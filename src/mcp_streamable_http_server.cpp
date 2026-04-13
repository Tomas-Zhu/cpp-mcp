/**
 * @file mcp_streamable_http_server.cpp
 * @brief Implementation of the MCP Streamable HTTP Server
 * 
 * Faithfully ported from the C# MCP SDK (modelcontextprotocol/csharp-sdk)
 * StreamableHttpServerTransport, StreamableHttpHandler, and StatefulSessionManager.
 */

#include "mcp_streamable_http_server.h"
#include <sstream>
#include <algorithm>
#include <random>

namespace mcp {

// ============================================================================
// sse_writer implementation
// ============================================================================

sse_writer::~sse_writer() {
    complete();
}

void sse_writer::set_sink(httplib::DataSink* sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = sink;
}

bool sse_writer::write_event(const std::string& event, const std::string& data, const std::string& id) {
    if (completed_.load(std::memory_order_acquire)) {
        return false;
    }

    std::stringstream ss;
    if (!id.empty()) {
        ss << "id: " << id << "\r\n";
    }
    if (!event.empty()) {
        ss << "event: " << event << "\r\n";
    }
    ss << "data: " << data << "\r\n\r\n";

    std::lock_guard<std::mutex> lock(mutex_);
    pending_events_.push(ss.str());
    cv_.notify_one();
    return true;
}

bool sse_writer::write_raw(const std::string& raw) {
    if (completed_.load(std::memory_order_acquire)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    pending_events_.push(raw);
    cv_.notify_one();
    return true;
}

void sse_writer::complete() {
    bool was = completed_.exchange(true, std::memory_order_release);
    if (!was) {
        cv_.notify_all();
    }
}

bool sse_writer::is_completed() const {
    return completed_.load(std::memory_order_acquire);
}

bool sse_writer::wait_and_flush(httplib::DataSink* sink, std::chrono::milliseconds timeout) {
    if (!sink || completed_.load(std::memory_order_acquire)) {
        return false;
    }

    std::string event_data;
    {
        std::unique_lock<std::mutex> lock(mutex_);

        if (completed_.load(std::memory_order_acquire)) {
            return false;
        }

        // Wait for events or completion
        bool got_event = cv_.wait_for(lock, timeout, [this] {
            return !pending_events_.empty() || completed_.load(std::memory_order_acquire);
        });

        if (completed_.load(std::memory_order_acquire) && pending_events_.empty()) {
            return false;
        }

        if (!got_event || pending_events_.empty()) {
            return true; // Timeout, but not closed - keep alive
        }

        // Drain all pending events into a single write
        while (!pending_events_.empty()) {
            event_data += pending_events_.front();
            pending_events_.pop();
        }
    }

    if (!event_data.empty()) {
        try {
            if (!sink->write(event_data.data(), event_data.size())) {
                complete();
                return false;
            }
        } catch (...) {
            complete();
            return false;
        }
    }

    return true;
}

// ============================================================================
// streamable_http_session implementation
// ============================================================================

streamable_http_session::streamable_http_session(const std::string& session_id)
    : session_id_(session_id) {
}

streamable_http_session::~streamable_http_session() {
    dispose();
}

streamable_http_session::state streamable_http_session::get_state() const {
    return state_.load(std::memory_order_acquire);
}

void streamable_http_session::start() {
    state expected = state::uninitialized;
    state_.compare_exchange_strong(expected, state::started, std::memory_order_release);
}

void streamable_http_session::dispose() {
    state prev = state_.exchange(state::disposed, std::memory_order_release);
    if (prev == state::disposed) {
        return; // Already disposed
    }

    // Close the GET stream if active
    close_get_stream();
}

bool streamable_http_session::is_disposed() const {
    return state_.load(std::memory_order_acquire) == state::disposed;
}

bool streamable_http_session::acquire_reference() {
    if (is_disposed()) {
        return false;
    }
    active_refs_.fetch_add(1, std::memory_order_relaxed);
    update_activity();
    return true;
}

void streamable_http_session::release_reference() {
    int prev = active_refs_.fetch_sub(1, std::memory_order_relaxed);
    if (prev <= 0) {
        active_refs_.store(0, std::memory_order_relaxed); // Don't go negative
    }
    update_activity();
}

int streamable_http_session::active_references() const {
    return active_refs_.load(std::memory_order_relaxed);
}

bool streamable_http_session::is_idle() const {
    return active_refs_.load(std::memory_order_relaxed) <= 0;
}

std::chrono::steady_clock::time_point streamable_http_session::last_activity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_activity_;
}

void streamable_http_session::update_activity() {
    std::lock_guard<std::mutex> lock(mutex_);
    last_activity_ = std::chrono::steady_clock::now();
}

bool streamable_http_session::is_initialized() const {
    return initialized_.load(std::memory_order_acquire);
}

void streamable_http_session::set_initialized(bool initialized) {
    initialized_.store(initialized, std::memory_order_release);
}

std::shared_ptr<sse_writer> streamable_http_session::try_start_get_stream() {
    // C# TryStartGetRequest: only one GET per session
    bool expected = false;
    if (!get_stream_active_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return nullptr; // Already has an active GET stream
    }

    std::lock_guard<std::mutex> lock(mutex_);
    get_stream_writer_ = std::make_shared<sse_writer>();
    return get_stream_writer_;
}

void streamable_http_session::close_get_stream() {
    std::shared_ptr<sse_writer> writer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        writer = get_stream_writer_;
        get_stream_writer_.reset();
    }
    get_stream_active_.store(false, std::memory_order_release);

    if (writer) {
        writer->complete();
    }
}

bool streamable_http_session::has_get_stream() const {
    return get_stream_active_.load(std::memory_order_acquire);
}

bool streamable_http_session::send_unsolicited_message(const json& message) {
    std::shared_ptr<sse_writer> writer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        writer = get_stream_writer_;
    }

    if (!writer || writer->is_completed()) {
        LOG_WARNING("No active GET SSE stream for session: ", session_id_);
        return false;
    }

    return writer->write_event("message", message.dump());
}

void streamable_http_session::set_client_info(const std::string& name, const std::string& version) {
    std::lock_guard<std::mutex> lock(mutex_);
    client_name_ = name;
    client_version_ = version;
}

std::string streamable_http_session::client_name() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return client_name_;
}

std::string streamable_http_session::client_version() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return client_version_;
}

// ============================================================================
// session_reference_guard implementation
// ============================================================================

session_reference_guard::session_reference_guard(std::shared_ptr<streamable_http_session> session)
    : session_(std::move(session)) {
    if (session_ && !session_->acquire_reference()) {
        session_.reset(); // Session is disposed, invalidate
    }
}

session_reference_guard::~session_reference_guard() {
    if (session_) {
        session_->release_reference();
    }
}

session_reference_guard::session_reference_guard(session_reference_guard&& other) noexcept
    : session_(std::move(other.session_)) {
}

session_reference_guard& session_reference_guard::operator=(session_reference_guard&& other) noexcept {
    if (this != &other) {
        if (session_) {
            session_->release_reference();
        }
        session_ = std::move(other.session_);
    }
    return *this;
}

// ============================================================================
// streamable_http_server implementation
// ============================================================================

streamable_http_server::streamable_http_server(const configuration& conf)
    : host_(conf.host)
    , port_(conf.port)
    , name_(conf.name)
    , version_(conf.version)
    , endpoint_(conf.endpoint)
    , stateless_(conf.stateless)
    , idle_timeout_(conf.idle_timeout)
    , max_idle_sessions_(conf.max_idle_sessions)
    , idle_check_interval_(conf.idle_check_interval)
    , enable_legacy_sse_(conf.enable_legacy_sse)
    , legacy_sse_endpoint_(conf.legacy_sse_endpoint)
    , legacy_msg_endpoint_(conf.legacy_msg_endpoint)
    , thread_pool_(conf.threadpool_size)
{
    // C# validates: stateless + legacy SSE is not allowed
    if (stateless_ && enable_legacy_sse_) {
        LOG_ERROR("Legacy SSE endpoints cannot be enabled in stateless mode");
        enable_legacy_sse_ = false;
    }

#ifdef MCP_SSL
    if (conf.ssl.server_cert_path && conf.ssl.server_private_key_path) {
        http_server_ = std::make_unique<httplib::SSLServer>(
            conf.ssl.server_cert_path->c_str(),
            conf.ssl.server_private_key_path->c_str());
    } else {
        http_server_ = std::make_unique<httplib::Server>();
    }
#else
    http_server_ = std::make_unique<httplib::Server>();
#endif
}

streamable_http_server::~streamable_http_server() {
    stop();
}

bool streamable_http_server::start(bool blocking) {
    if (running_.load()) {
        return true;
    }

    LOG_INFO("Starting MCP Streamable HTTP server on ", host_, ":", port_);

    // Setup CORS
    setup_cors();

    // ========================================================================
    // Map the three HTTP methods to the endpoint path
    // This corresponds to C# MapMcp() which sets up POST/GET/DELETE on a route group
    // ========================================================================

    // POST endpoint - receives JSON-RPC requests/notifications
    http_server_->Post(endpoint_.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
        this->handle_post(req, res);
        LOG_INFO(req.remote_addr, ":", req.remote_port, " - \"POST ", req.path, " HTTP/1.1\" ", res.status);
    });

    // GET endpoint - opens SSE stream for unsolicited messages (stateful mode only)
    // C#: if (!options.Stateless) { group.MapGet(...) }
    if (!stateless_) {
        http_server_->Get(endpoint_.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
            this->handle_get(req, res);
            LOG_INFO(req.remote_addr, ":", req.remote_port, " - \"GET ", req.path, " HTTP/1.1\" ", res.status);
        });

        // DELETE endpoint - client-initiated session termination (stateful mode only)
        // C#: if (!options.Stateless) { group.MapDelete(...) }
        http_server_->Delete(endpoint_.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
            this->handle_delete(req, res);
            LOG_INFO(req.remote_addr, ":", req.remote_port, " - \"DELETE ", req.path, " HTTP/1.1\" ", res.status);
        });
    }

    // ========================================================================
    // Legacy SSE endpoints (backward compatibility)
    // C#: if (options.EnableLegacySse) { ... }
    // ========================================================================
    if (enable_legacy_sse_ && !stateless_) {
        http_server_->Get(legacy_sse_endpoint_.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
            this->handle_legacy_sse(req, res);
            LOG_INFO(req.remote_addr, ":", req.remote_port, " - \"GET ", req.path, " HTTP/1.1\" (legacy SSE) ", res.status);
        });

        http_server_->Post(legacy_msg_endpoint_.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
            this->handle_legacy_message(req, res);
            LOG_INFO(req.remote_addr, ":", req.remote_port, " - \"POST ", req.path, " HTTP/1.1\" (legacy message) ", res.status);
        });
    }

    // ========================================================================
    // Start idle session tracking thread (C# IdleTrackingBackgroundService)
    // Only needed in stateful mode
    // ========================================================================
    if (!stateless_) {
        idle_thread_run_ = true;
        idle_tracking_thread_ = std::make_unique<std::thread>([this]() {
            this->idle_tracking_loop();
        });
    }

    // Start HTTP server
    if (blocking) {
        running_.store(true);
        LOG_INFO("Starting Streamable HTTP server in blocking mode");
        if (!http_server_->listen(host_.c_str(), port_)) {
            running_.store(false);
            LOG_ERROR("Failed to start server on ", host_, ":", port_);
            return false;
        }
        return true;
    } else {
        server_thread_ = std::make_unique<std::thread>([this]() {
            LOG_INFO("Starting Streamable HTTP server in separate thread");
            if (!http_server_->listen(host_.c_str(), port_)) {
                LOG_ERROR("Failed to start server on ", host_, ":", port_);
                running_.store(false);
            }
        });
        running_.store(true);
        return true;
    }
}

void streamable_http_server::stop() {
    if (!running_.load()) {
        return;
    }

    LOG_INFO("Stopping MCP Streamable HTTP server on ", host_, ":", port_);
    running_.store(false);

    // Stop idle tracking thread
    if (idle_tracking_thread_ && idle_tracking_thread_->joinable()) {
        {
            std::lock_guard<std::mutex> lock(idle_mutex_);
            idle_thread_run_ = false;
        }
        idle_cv_.notify_one();
        try {
            idle_tracking_thread_->join();
        } catch (...) {
            idle_tracking_thread_->detach();
        }
    }

    // Dispose all sessions (C# DisposeAllSessionsAsync)
    dispose_all_sessions();

    // Close legacy SSE dispatchers
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (auto& [_, dispatcher] : legacy_dispatchers_) {
            if (dispatcher) {
                dispatcher->close();
            }
        }
        legacy_dispatchers_.clear();

        for (auto& [_, thread] : legacy_sse_threads_) {
            if (thread && thread->joinable()) {
                thread->detach();
            }
        }
        legacy_sse_threads_.clear();
    }

    // Stop HTTP server
    if (server_thread_ && server_thread_->joinable()) {
        http_server_->stop();
        try {
            server_thread_->join();
        } catch (...) {
            server_thread_->detach();
        }
    } else {
        http_server_->stop();
    }

    LOG_INFO("MCP Streamable HTTP server stopped");
}

bool streamable_http_server::is_running() const {
    return running_.load();
}

void streamable_http_server::set_server_info(const std::string& name, const std::string& version) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    name_ = name;
    version_ = version;
}

void streamable_http_server::set_capabilities(const json& capabilities) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    capabilities_ = capabilities;
}

void streamable_http_server::register_method(const std::string& method, method_handler handler) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    method_handlers_[method] = handler;
}

void streamable_http_server::register_notification(const std::string& method, notification_handler handler) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    notification_handlers_[method] = handler;
}

void streamable_http_server::register_resource(const std::string& path, std::shared_ptr<resource> res) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    resources_[path] = res;

    // Auto-register resource method handlers (same pattern as existing server)
    if (method_handlers_.find("resources/read") == method_handlers_.end()) {
        method_handlers_["resources/read"] = [this](const json& params, const std::string& /*session_id*/) -> json {
            if (!params.contains("uri")) {
                throw mcp_exception(error_code::invalid_params, "Missing 'uri' parameter");
            }
            std::string uri = params["uri"];
            std::lock_guard<std::mutex> lock(handlers_mutex_);
            auto it = resources_.find(uri);
            if (it == resources_.end()) {
                throw mcp_exception(error_code::invalid_params, "Resource not found: " + uri);
            }
            json contents = json::array();
            contents.push_back(it->second->read());
            return json{{"contents", contents}};
        };
    }

    if (method_handlers_.find("resources/list") == method_handlers_.end()) {
        method_handlers_["resources/list"] = [this](const json& params, const std::string& /*session_id*/) -> json {
            std::lock_guard<std::mutex> lock(handlers_mutex_);
            json resources_json = json::array();
            for (const auto& [uri, r] : resources_) {
                resources_json.push_back(r->get_metadata());
            }
            json result = {{"resources", resources_json}};
            if (params.contains("cursor")) {
                result["nextCursor"] = "";
            }
            return result;
        };
    }

    if (method_handlers_.find("resources/subscribe") == method_handlers_.end()) {
        method_handlers_["resources/subscribe"] = [this](const json& params, const std::string& /*session_id*/) -> json {
            if (!params.contains("uri")) {
                throw mcp_exception(error_code::invalid_params, "Missing 'uri' parameter");
            }
            std::string uri = params["uri"];
            std::lock_guard<std::mutex> lock(handlers_mutex_);
            auto it = resources_.find(uri);
            if (it == resources_.end()) {
                throw mcp_exception(error_code::invalid_params, "Resource not found: " + uri);
            }
            return json::object();
        };
    }

    if (method_handlers_.find("resources/templates/list") == method_handlers_.end()) {
        method_handlers_["resources/templates/list"] = [](const json& /*params*/, const std::string& /*session_id*/) -> json {
            return json::array();
        };
    }
}

void streamable_http_server::register_tool(const tool& t, tool_handler handler) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    tools_[t.name] = std::make_pair(t, handler);

    // Auto-register tool method handlers (same pattern as existing server)
    if (method_handlers_.find("tools/list") == method_handlers_.end()) {
        method_handlers_["tools/list"] = [this](const json& /*params*/, const std::string& /*session_id*/) -> json {
            std::lock_guard<std::mutex> lock(handlers_mutex_);
            json tools_json = json::array();
            for (const auto& [name, tool_pair] : tools_) {
                tools_json.push_back(tool_pair.first.to_json());
            }
            return json{{"tools", tools_json}};
        };
    }

    if (method_handlers_.find("tools/call") == method_handlers_.end()) {
        method_handlers_["tools/call"] = [this](const json& params, const std::string& session_id) -> json {
            if (!params.contains("name")) {
                throw mcp_exception(error_code::invalid_params, "Missing 'name' parameter");
            }

            std::string tool_name = params["name"];
            tool_handler tool_fn;
            {
                std::lock_guard<std::mutex> lock(handlers_mutex_);
                auto it = tools_.find(tool_name);
                if (it == tools_.end()) {
                    throw mcp_exception(error_code::invalid_params, "Tool not found: " + tool_name);
                }
                tool_fn = it->second.second;
            }

            json tool_args = params.contains("arguments") ? params["arguments"] : json::array();
            if (tool_args.is_string()) {
                try {
                    tool_args = json::parse(tool_args.get<std::string>());
                } catch (const json::exception& e) {
                    throw mcp_exception(error_code::invalid_params, "Invalid JSON arguments: " + std::string(e.what()));
                }
            }

            json tool_result = {{"isError", false}};
            try {
                tool_result["content"] = tool_fn(tool_args, session_id);
            } catch (const std::exception& e) {
                tool_result["isError"] = true;
                tool_result["content"] = json::array({{{"type", "text"}, {"text", e.what()}}});
            }
            return tool_result;
        };
    }
}

void streamable_http_server::register_session_cleanup(const std::string& key, session_cleanup_handler handler) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    session_cleanup_handlers_[key] = handler;
}

std::vector<tool> streamable_http_server::get_tools() const {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    std::vector<tool> result;
    for (const auto& [name, tool_pair] : tools_) {
        result.push_back(tool_pair.first);
    }
    return result;
}

void streamable_http_server::set_auth_handler(auth_handler handler) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    auth_handler_ = handler;
}

void streamable_http_server::send_request(const std::string& session_id, const request& req) {
    auto session = find_session(session_id);
    if (!session) {
        LOG_ERROR("Cannot send to non-existent session: ", session_id);
        return;
    }

    if (!session->send_unsolicited_message(req.to_json())) {
        LOG_WARNING("Failed to send unsolicited message to session: ", session_id);
    }
}

void streamable_http_server::broadcast(const request& req) {
    std::vector<std::shared_ptr<streamable_http_session>> all_sessions;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (auto& [_, session] : sessions_) {
            if (session && !session->is_disposed() && session->is_initialized()) {
                all_sessions.push_back(session);
            }
        }
    }

    json msg = req.to_json();
    for (auto& session : all_sessions) {
        session->send_unsolicited_message(msg);
    }
}

bool streamable_http_server::set_mount_point(const std::string& mount_point, const std::string& dir,
                                              httplib::Headers headers) {
    return http_server_->set_mount_point(mount_point, dir, headers);
}

// ============================================================================
// CORS setup
// ============================================================================

void streamable_http_server::setup_cors() {
    http_server_->Options(".*", [](const httplib::Request& /*req*/, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers",
                        "Content-Type, Accept, Mcp-Session-Id, Mcp-Protocol-Version, Last-Event-ID");
        res.set_header("Access-Control-Expose-Headers", "Mcp-Session-Id");
        res.status = 204;
    });
}

// ============================================================================
// POST handler
// Corresponds to C# HandlePostRequestAsync
// 
// This is the core of the Streamable HTTP transport. The key insight from the C# SDK:
// - Each POST creates a per-request transport (StreamableHttpPostTransport)
// - For JSON-RPC requests (with ID): the response is written back on the SAME
//   HTTP POST response as an SSE event stream, not via the GET SSE stream
// - For notifications (no ID): returns 202 Accepted immediately
// - The "initialize" request creates a new session; subsequent requests must
//   include the Mcp-Session-Id header
// ============================================================================

void streamable_http_server::handle_post(const httplib::Request& req, httplib::Response& res) {
    // CORS headers
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Expose-Headers", "Mcp-Session-Id");

    // ---- Step 1: Validate Accept header ----
    // C#: Validate that Accept includes both application/json and text/event-stream
    if (!validate_accept_header(req, res)) {
        return;
    }

    // ---- Step 2: Parse JSON-RPC message from request body ----
    json req_json;
    try {
        req_json = json::parse(req.body);
    } catch (const json::exception& e) {
        LOG_ERROR("Failed to parse JSON request: ", e.what());
        res.status = 400;
        json error_resp = {
            {"jsonrpc", "2.0"},
            {"error", {{"code", static_cast<int>(error_code::parse_error)}, {"message", "Parse error"}}},
            {"id", nullptr}
        };
        res.set_content(error_resp.dump(), "application/json");
        return;
    }

    // ---- Step 3: Create request object ----
    request mcp_req;
    try {
        mcp_req.jsonrpc = req_json.value("jsonrpc", "2.0");
        if (req_json.contains("id") && !req_json["id"].is_null()) {
            mcp_req.id = req_json["id"];
        }
        if (req_json.contains("method")) {
            mcp_req.method = req_json["method"].get<std::string>();
        }
        if (req_json.contains("params")) {
            mcp_req.params = req_json["params"];
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to create request object: ", e.what());
        res.status = 400;
        json error_resp = {
            {"jsonrpc", "2.0"},
            {"error", {{"code", static_cast<int>(error_code::invalid_request)}, {"message", "Invalid request format"}}},
            {"id", nullptr}
        };
        res.set_content(error_resp.dump(), "application/json");
        return;
    }

    // ---- Step 4: Get or create session ----
    // C#: GetOrCreateSessionAsync
    bool is_new_session = false;
    auto session = get_or_create_session(req, res, req_json, is_new_session);
    if (!session) {
        return; // Error response already set by get_or_create_session
    }

    // Acquire reference (C# AcquireReferenceAsync)
    session_reference_guard ref_guard(session);
    if (!ref_guard.is_valid()) {
        res.status = 404;
        res.set_content("{\"error\":\"Session is disposed\"}", "application/json");
        return;
    }

    // Set Mcp-Session-Id header on response (C#: always set for stateful mode)
    if (!stateless_) {
        res.set_header("Mcp-Session-Id", session->id());
    }

    // ---- Step 5: Handle the message ----
    // C# StreamableHttpPostTransport pattern:
    // - Notifications -> write to channel, return 202
    // - Requests -> write to channel, block until response, write response as SSE to POST body

    if (mcp_req.is_notification()) {
        // ---- Notification handling ----
        // C#: For notifications, write to channel and return false (-> 202 Accepted)
        std::string session_id = session->id();
        thread_pool_.enqueue([this, mcp_req, session_id]() {
            process_request(mcp_req, session_id);
        });

        res.status = 202;
        res.set_content("Accepted", "text/plain");
        return;
    }

    // ---- Request handling (has ID) ----
    // C# StreamableHttpPostTransport: response flows back on the SAME POST response as SSE
    // This is the key architectural difference from the old SSE transport

    // Process the request synchronously (or with a future for timeout support)
    std::string session_id = session->id();

    // Use a promise/future to get the response from the thread pool
    auto response_promise = std::make_shared<std::promise<json>>();
    auto response_future = response_promise->get_future();

    thread_pool_.enqueue([this, mcp_req, session_id, response_promise]() {
        try {
            json result = process_request(mcp_req, session_id);
            response_promise->set_value(result);
        } catch (const std::exception& e) {
            try {
                json err = response::create_error(
                    mcp_req.id, error_code::internal_error,
                    "Internal error: " + std::string(e.what())
                ).to_json();
                response_promise->set_value(err);
            } catch (...) {
                response_promise->set_exception(std::current_exception());
            }
        } catch (...) {
            try {
                json err = response::create_error(
                    mcp_req.id, error_code::internal_error,
                    "Unknown internal error"
                ).to_json();
                response_promise->set_value(err);
            } catch (...) {
                response_promise->set_exception(std::current_exception());
            }
        }
    });

    // Wait for the response (with timeout)
    json response_json;
    try {
        auto status = response_future.wait_for(std::chrono::seconds(300)); // 5 minute timeout
        if (status == std::future_status::ready) {
            response_json = response_future.get();
        } else {
            response_json = response::create_error(
                mcp_req.id, error_code::internal_error,
                "Request timed out"
            ).to_json();
        }
    } catch (const std::exception& e) {
        response_json = response::create_error(
            mcp_req.id, error_code::internal_error,
            "Error processing request: " + std::string(e.what())
        ).to_json();
    }

    // ---- Write response as SSE on POST response body ----
    // C# StreamableHttpPostTransport.SendMessageAsync writes SSE to the POST response
    // The Content-Type is text/event-stream
    res.status = 200;
    std::stringstream sse_body;
    sse_body << "event: message\r\ndata: " << response_json.dump() << "\r\n\r\n";
    res.set_content(sse_body.str(), "text/event-stream");
}

// ============================================================================
// GET handler
// Corresponds to C# HandleGetRequestAsync
// 
// Opens an SSE stream for unsolicited server-to-client messages.
// Only one GET stream per session is allowed (C# TryStartGetRequest).
// This stream is used for:
//   - Server-initiated notifications
//   - Sampling/tool-calling requests from server to client
// ============================================================================

void streamable_http_server::handle_get(const httplib::Request& req, httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Expose-Headers", "Mcp-Session-Id");

    // ---- Validate Accept header ----
    // C#: GET must accept text/event-stream
    auto accept_it = req.headers.find("Accept");
    if (accept_it == req.headers.end() || accept_it->second.find("text/event-stream") == std::string::npos) {
        res.status = 406; // Not Acceptable
        res.set_content("{\"error\":\"Accept header must include text/event-stream\"}", "application/json");
        return;
    }

    // ---- Look up session ----
    std::string session_id = get_session_id_from_header(req);
    if (session_id.empty()) {
        res.status = 400;
        res.set_content("{\"error\":\"Missing Mcp-Session-Id header\"}", "application/json");
        return;
    }

    auto session = find_session(session_id);
    if (!session) {
        res.status = 404;
        res.set_content("{\"error\":\"Session not found\"}", "application/json");
        return;
    }

    // Acquire reference
    session_reference_guard ref_guard(session);
    if (!ref_guard.is_valid()) {
        res.status = 404;
        res.set_content("{\"error\":\"Session is disposed\"}", "application/json");
        return;
    }

    res.set_header("Mcp-Session-Id", session_id);

    // ---- Try to start GET stream (C# TryStartGetRequest) ----
    auto writer = session->try_start_get_stream();
    if (!writer) {
        // C#: Only one GET stream per session
        res.status = 409; // Conflict
        res.set_content("{\"error\":\"A GET SSE stream is already active for this session\"}", "application/json");
        return;
    }

    // ---- Set up SSE chunked response ----
    res.set_header("Content-Type", "text/event-stream");
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");

    // Use httplib's chunked content provider to keep the connection open
    // The writer will block in wait_and_flush until events arrive or the stream is closed
    res.set_chunked_content_provider("text/event-stream",
        [session, writer](size_t /*offset*/, httplib::DataSink& sink) -> bool {
            if (session->is_disposed() || writer->is_completed()) {
                return false;
            }

            bool result = writer->wait_and_flush(&sink);
            if (!result) {
                session->close_get_stream();
                return false;
            }

            return true;
        });
}

// ============================================================================
// DELETE handler
// Corresponds to C# HandleDeleteRequestAsync
// 
// Client-initiated session termination. Disposes the session and
// cleans up all associated resources.
// ============================================================================

void streamable_http_server::handle_delete(const httplib::Request& req, httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");

    // ---- Look up session ----
    std::string session_id = get_session_id_from_header(req);
    if (session_id.empty()) {
        res.status = 400;
        res.set_content("{\"error\":\"Missing Mcp-Session-Id header\"}", "application/json");
        return;
    }

    auto session = find_session(session_id);
    if (!session) {
        res.status = 404;
        res.set_content("{\"error\":\"Session not found\"}", "application/json");
        return;
    }

    LOG_INFO("Client requested session termination: ", session_id);

    // Remove and dispose the session
    remove_session(session_id);

    res.status = 200;
    res.set_content("{\"status\":\"session terminated\"}", "application/json");
}

// ============================================================================
// Legacy SSE handlers (backward compatibility with old SSE transport)
// Corresponds to C# EnableLegacySse option
// ============================================================================

void streamable_http_server::handle_legacy_sse(const httplib::Request& /*req*/, httplib::Response& res) {
    std::string session_id = generate_session_id();
    std::string session_uri = legacy_msg_endpoint_ + "?session_id=" + session_id;

    // Create a session for legacy mode
    auto session = std::make_shared<streamable_http_session>(session_id);
    session->start();
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_[session_id] = session;
    }

    // Create legacy event dispatcher
    auto dispatcher = std::make_shared<event_dispatcher>();
    dispatcher->update_activity();
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        legacy_dispatchers_[session_id] = dispatcher;
    }

    // SSE heartbeat thread (same as old server)
    auto thread = std::make_unique<std::thread>([this, session_id, session_uri, dispatcher]() {
        try {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::stringstream ss;
            ss << "event: endpoint\r\ndata: " << session_uri << "\r\n\r\n";
            dispatcher->send_event(ss.str());
            dispatcher->update_activity();

            int heartbeat_count = 0;
            while (running_.load() && !dispatcher->is_closed()) {
                std::this_thread::sleep_for(std::chrono::seconds(5) + std::chrono::milliseconds(rand() % 500));
                if (dispatcher->is_closed() || !running_.load()) break;

                std::stringstream heartbeat;
                heartbeat << "event: heartbeat\r\ndata: " << heartbeat_count++ << "\r\n\r\n";
                if (!dispatcher->send_event(heartbeat.str())) break;
                dispatcher->update_activity();
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Legacy SSE thread exception: ", session_id, ", ", e.what());
        }
        remove_session(session_id);
    });

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        legacy_sse_threads_[session_id] = std::move(thread);
    }

    // Setup chunked content provider
    res.set_header("Content-Type", "text/event-stream");
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");
    res.set_header("Access-Control-Allow-Origin", "*");

    res.set_chunked_content_provider("text/event-stream",
        [dispatcher](size_t /*offset*/, httplib::DataSink& sink) -> bool {
            if (dispatcher->is_closed()) return false;
            dispatcher->update_activity();
            bool result = dispatcher->wait_event(&sink);
            if (!result) return false;
            dispatcher->update_activity();
            return true;
        });
}

void streamable_http_server::handle_legacy_message(const httplib::Request& req, httplib::Response& res) {
    res.set_header("Content-Type", "application/json");
    res.set_header("Access-Control-Allow-Origin", "*");

    // Get session ID from query params
    auto it = req.params.find("session_id");
    std::string session_id = it != req.params.end() ? it->second : "";

    if (session_id.empty()) {
        res.status = 400;
        res.set_content("{\"error\":\"Missing session_id parameter\"}", "application/json");
        return;
    }

    // Find legacy dispatcher
    std::shared_ptr<event_dispatcher> dispatcher;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto disp_it = legacy_dispatchers_.find(session_id);
        if (disp_it == legacy_dispatchers_.end()) {
            res.status = 404;
            res.set_content("{\"error\":\"Session not found\"}", "application/json");
            return;
        }
        dispatcher = disp_it->second;
    }
    dispatcher->update_activity();

    // Parse request
    json req_json;
    try {
        req_json = json::parse(req.body);
    } catch (const json::exception& /*e*/) {
        res.status = 400;
        res.set_content("{\"error\":\"Invalid JSON\"}", "application/json");
        return;
    }

    request mcp_req;
    try {
        mcp_req.jsonrpc = req_json.value("jsonrpc", "2.0");
        if (req_json.contains("id") && !req_json["id"].is_null()) {
            mcp_req.id = req_json["id"];
        }
        if (req_json.contains("method")) {
            mcp_req.method = req_json["method"].get<std::string>();
        }
        if (req_json.contains("params")) {
            mcp_req.params = req_json["params"];
        }
    } catch (const std::exception& /*e*/) {
        res.status = 400;
        res.set_content("{\"error\":\"Invalid request format\"}", "application/json");
        return;
    }

    // Notifications -> 202
    if (mcp_req.is_notification()) {
        thread_pool_.enqueue([this, mcp_req, session_id]() {
            process_request(mcp_req, session_id);
        });
        res.status = 202;
        res.set_content("Accepted", "text/plain");
        return;
    }

    // Requests -> process and send via legacy SSE
    thread_pool_.enqueue([this, mcp_req, session_id, dispatcher]() {
        json response_json = process_request(mcp_req, session_id);
        std::stringstream ss;
        ss << "event: message\r\ndata: " << response_json.dump() << "\r\n\r\n";
        dispatcher->send_event(ss.str());
    });

    res.status = 202;
    res.set_content("Accepted", "text/plain");
}

// ============================================================================
// Validation helpers
// ============================================================================

bool streamable_http_server::validate_accept_header(const httplib::Request& req, httplib::Response& res) {
    // C# ValidateAcceptHeader: POST must accept both application/json and text/event-stream
    auto accept_it = req.headers.find("Accept");
    if (accept_it == req.headers.end()) {
        // Be lenient: if no Accept header, allow it (some clients don't send one)
        return true;
    }

    const std::string& accept = accept_it->second;

    // Check for wildcard
    if (accept.find("*/*") != std::string::npos) {
        return true;
    }

    // C# strictly requires both, but we're lenient and just warn
    bool has_json = accept.find("application/json") != std::string::npos;
    bool has_sse = accept.find("text/event-stream") != std::string::npos;

    if (!has_json && !has_sse) {
        res.status = 406; // Not Acceptable
        res.set_content(
            "{\"error\":\"Accept header must include application/json and/or text/event-stream\"}",
            "application/json");
        return false;
    }

    return true;
}

std::string streamable_http_server::validate_protocol_version_header(
    const httplib::Request& req, httplib::Response& res) {

    auto it = req.headers.find("Mcp-Protocol-Version");
    if (it != req.headers.end()) {
        return it->second;
    }
    // Protocol version header is optional for the initialize request
    return "";
}

std::string streamable_http_server::get_session_id_from_header(const httplib::Request& req) {
    auto it = req.headers.find("Mcp-Session-Id");
    if (it != req.headers.end()) {
        return it->second;
    }
    return "";
}

// ============================================================================
// Session management
// Corresponds to C# GetOrCreateSessionAsync, StatefulSessionManager
// ============================================================================

std::shared_ptr<streamable_http_session> streamable_http_server::get_or_create_session(
    const httplib::Request& req, httplib::Response& res,
    const json& message, bool& is_new_session) {

    is_new_session = false;
    std::string session_id = get_session_id_from_header(req);

    if (stateless_) {
        // C# Stateless mode: create ephemeral session for every request
        auto session = std::make_shared<streamable_http_session>(generate_session_id());
        session->start();
        is_new_session = true;
        // In stateless mode, don't track sessions - they live only for this request
        return session;
    }

    // ---- Stateful mode ----

    // Check if this is an initialize request (no session ID expected)
    bool is_initialize = message.contains("method") && message["method"] == "initialize";

    if (session_id.empty()) {
        if (is_initialize) {
            // C#: No session ID + initialize request -> StartNewSessionAsync
            auto session = create_session();
            is_new_session = true;
            return session;
        } else {
            // C#: No session ID + non-initialize request -> error
            // Exception: handle ping without session
            if (message.contains("method") && message["method"] == "ping") {
                auto session = std::make_shared<streamable_http_session>(generate_session_id());
                session->start();
                session->set_initialized(true);
                return session;
            }

            res.status = 400;
            res.set_content("{\"error\":\"Missing Mcp-Session-Id header\"}", "application/json");
            return nullptr;
        }
    }

    // Has session ID -> look up existing session
    auto session = find_session(session_id);
    if (!session) {
        // C#: Session not found -> 404
        res.status = 404;
        res.set_content("{\"error\":\"Session not found. The session may have expired.\"}", "application/json");
        return nullptr;
    }

    if (session->is_disposed()) {
        res.status = 404;
        res.set_content("{\"error\":\"Session is disposed\"}", "application/json");
        return nullptr;
    }

    return session;
}

std::shared_ptr<streamable_http_session> streamable_http_server::create_session() {
    std::string session_id = generate_session_id();
    auto session = std::make_shared<streamable_http_session>(session_id);
    session->start();

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_[session_id] = session;
    }

    LOG_INFO("Created new session: ", session_id);
    return session;
}

std::shared_ptr<streamable_http_session> streamable_http_server::find_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        return it->second;
    }
    return nullptr;
}

void streamable_http_server::remove_session(const std::string& session_id) {
    std::shared_ptr<streamable_http_session> session;
    std::shared_ptr<event_dispatcher> legacy_dispatcher;
    std::unique_ptr<std::thread> legacy_thread;

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);

        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            session = it->second;
            sessions_.erase(it);
        }

        auto disp_it = legacy_dispatchers_.find(session_id);
        if (disp_it != legacy_dispatchers_.end()) {
            legacy_dispatcher = disp_it->second;
            legacy_dispatchers_.erase(disp_it);
        }

        auto thread_it = legacy_sse_threads_.find(session_id);
        if (thread_it != legacy_sse_threads_.end()) {
            legacy_thread = std::move(thread_it->second);
            legacy_sse_threads_.erase(thread_it);
        }
    }

    // Run session cleanup handlers
    {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        for (const auto& [key, handler] : session_cleanup_handlers_) {
            try {
                handler(session_id);
            } catch (const std::exception& e) {
                LOG_WARNING("Session cleanup handler error: ", key, ", ", e.what());
            }
        }
    }

    // Dispose session
    if (session) {
        session->dispose();
    }

    // Close legacy dispatcher
    if (legacy_dispatcher) {
        legacy_dispatcher->close();
    }

    // Detach legacy thread
    if (legacy_thread && legacy_thread->joinable()) {
        legacy_thread->detach();
    }

    LOG_INFO("Removed session: ", session_id);
}

std::string streamable_http_server::generate_session_id() const {
    // C# uses RandomNumberGenerator.Fill(16 bytes) + Base64Url encoding
    // We use a similar approach with random_device + mt19937
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);

    std::stringstream ss;
    ss << std::hex;

    // Generate UUID-like format: 8-4-4-4-12
    for (int i = 0; i < 8; ++i) ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 4; ++i) ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 4; ++i) ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 4; ++i) ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 12; ++i) ss << dis(gen);

    return ss.str();
}

// ============================================================================
// Request processing
// ============================================================================

json streamable_http_server::process_request(const request& req, const std::string& session_id) {
    // Handle notifications
    if (req.is_notification()) {
        if (req.method == "notifications/initialized") {
            auto session = find_session(session_id);
            if (session) {
                session->set_initialized(true);
                LOG_INFO("Session initialized: ", session_id);
            }
        }

        // Look up notification handler
        notification_handler handler;
        {
            std::lock_guard<std::mutex> lock(handlers_mutex_);
            auto it = notification_handlers_.find(req.method);
            if (it != notification_handlers_.end()) {
                handler = it->second;
            }
        }

        if (handler) {
            try {
                handler(req.params, session_id);
            } catch (const std::exception& e) {
                LOG_ERROR("Notification handler error: ", req.method, ", ", e.what());
            }
        }

        return json::object();
    }

    // Handle method calls
    try {
        LOG_INFO("Processing method call: ", req.method);

        // Special case: initialize
        if (req.method == "initialize") {
            return handle_initialize(req, session_id);
        } else if (req.method == "ping") {
            return response::create_success(req.id, json::object()).to_json();
        }

        // Check if session is initialized
        auto session = find_session(session_id);
        if (session && !session->is_initialized()) {
            LOG_WARNING("Session not initialized: ", session_id);
            return response::create_error(
                req.id, error_code::invalid_request,
                "Session not initialized"
            ).to_json();
        }

        // Find method handler
        method_handler handler;
        {
            std::lock_guard<std::mutex> lock(handlers_mutex_);
            auto it = method_handlers_.find(req.method);
            if (it != method_handlers_.end()) {
                handler = it->second;
            }
        }

        if (handler) {
            LOG_INFO("Calling method handler: ", req.method);
            json result = handler(req.params, session_id);
            LOG_INFO("Method call successful: ", req.method);
            return response::create_success(req.id, result).to_json();
        }

        // Method not found
        LOG_WARNING("Method not found: ", req.method);
        return response::create_error(
            req.id, error_code::method_not_found,
            "Method not found: " + req.method
        ).to_json();

    } catch (const mcp_exception& e) {
        LOG_ERROR("MCP exception: ", e.what(), ", code: ", static_cast<int>(e.code()));
        return response::create_error(req.id, e.code(), e.what()).to_json();
    } catch (const std::exception& e) {
        LOG_ERROR("Exception while processing request: ", e.what());
        return response::create_error(
            req.id, error_code::internal_error,
            "Internal error: " + std::string(e.what())
        ).to_json();
    } catch (...) {
        LOG_ERROR("Unknown exception while processing request");
        return response::create_error(
            req.id, error_code::internal_error,
            "Unknown internal error"
        ).to_json();
    }
}

json streamable_http_server::handle_initialize(const request& req, const std::string& session_id) {
    const json& params = req.params;

    // Version negotiation
    if (!params.contains("protocolVersion") || !params["protocolVersion"].is_string()) {
        LOG_ERROR("Missing or invalid protocolVersion parameter");
        return response::create_error(
            req.id, error_code::invalid_params,
            "Expected string for 'protocolVersion' parameter"
        ).to_json();
    }

    std::string requested_version = params["protocolVersion"].get<std::string>();
    LOG_INFO("Client requested protocol version: ", requested_version);

    // Extract client info
    std::string client_name = "UnknownClient";
    std::string client_version = "UnknownVersion";

    if (params.contains("clientInfo")) {
        if (params["clientInfo"].contains("name")) {
            client_name = params["clientInfo"]["name"];
        }
        if (params["clientInfo"].contains("version")) {
            client_version = params["clientInfo"]["version"];
        }
    }

    // Store client info in session
    auto session = find_session(session_id);
    if (session) {
        session->set_client_info(client_name, client_version);
    }

    LOG_INFO("Client connected: ", client_name, " ", client_version);

    // Build response
    json server_info = {
        {"name", name_},
        {"version", version_}
    };

    json result = {
        {"protocolVersion", MCP_VERSION},
        {"capabilities", capabilities_},
        {"serverInfo", server_info}
    };

    LOG_INFO("Initialization successful, waiting for notifications/initialized notification");
    return response::create_success(req.id, result).to_json();
}

// ============================================================================
// Idle session tracking
// Corresponds to C# IdleTrackingBackgroundService
// ============================================================================

void streamable_http_server::idle_tracking_loop() {
    LOG_INFO("Idle session tracking thread started");

    while (true) {
        std::unique_lock<std::mutex> lock(idle_mutex_);
        auto should_exit = idle_cv_.wait_for(lock, idle_check_interval_, [this] {
            return !idle_thread_run_;
        });

        if (should_exit) {
            LOG_INFO("Idle session tracking thread exiting");
            return;
        }
        lock.unlock();

        try {
            prune_idle_sessions();
        } catch (const std::exception& e) {
            LOG_ERROR("Exception in idle tracking: ", e.what());
        }
    }
}

void streamable_http_server::prune_idle_sessions() {
    // C# PruneIdleSessionsAsync: check idle sessions and dispose expired ones
    if (!running_.load()) return;

    auto now = std::chrono::steady_clock::now();
    std::vector<std::string> sessions_to_remove;

    // Count idle sessions and find expired ones
    size_t idle_count = 0;
    std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> idle_sessions;

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (const auto& [sid, session] : sessions_) {
            if (!session || session->is_disposed()) continue;

            if (session->is_idle()) {
                idle_count++;
                auto last = session->last_activity();

                // Check timeout
                if (now - last > idle_timeout_) {
                    sessions_to_remove.push_back(sid);
                } else {
                    idle_sessions.emplace_back(sid, last);
                }
            }
        }
    }

    // C#: If idle session count exceeds max, prune oldest first
    if (idle_count > max_idle_sessions_) {
        // Sort by last activity (oldest first)
        std::sort(idle_sessions.begin(), idle_sessions.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });

        size_t to_prune = idle_count - max_idle_sessions_;
        for (size_t i = 0; i < to_prune && i < idle_sessions.size(); ++i) {
            sessions_to_remove.push_back(idle_sessions[i].first);
        }
    }

    // Remove sessions
    for (const auto& sid : sessions_to_remove) {
        LOG_INFO("Pruning idle session: ", sid);
        remove_session(sid);
    }
}

void streamable_http_server::dispose_all_sessions() {
    // C#: DisposeAllSessionsAsync - called on shutdown
    std::vector<std::string> all_session_ids;

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (const auto& [sid, _] : sessions_) {
            all_session_ids.push_back(sid);
        }
    }

    for (const auto& sid : all_session_ids) {
        remove_session(sid);
    }

    LOG_INFO("All sessions disposed");
}

} // namespace mcp
