/**
 * @file mcp_streamable_http_server.h
 * @brief MCP Streamable HTTP Server implementation
 * 
 * This file implements the Streamable HTTP transport for the Model Context Protocol,
 * faithfully ported from the C# MCP SDK (modelcontextprotocol/csharp-sdk).
 * 
 * Key features (matching C# SDK StreamableHttpServerTransport):
 * - Three HTTP endpoints on the same path: POST (requests), GET (SSE stream), DELETE (session termination)
 * - Per-POST response correlation: responses flow back on the same HTTP POST response as SSE events
 * - Stateful vs Stateless modes
 * - Session management with Mcp-Session-Id header, reference counting, idle tracking
 * - Protocol version validation via Mcp-Protocol-Version header
 * - Accept header validation (POST must accept both application/json and text/event-stream)
 * - GET SSE stream for unsolicited server-to-client messages
 * - DELETE endpoint for client-initiated session termination
 * - Optional legacy SSE compatibility (/sse + /message endpoints)
 */

#ifndef MCP_STREAMABLE_HTTP_SERVER_H
#define MCP_STREAMABLE_HTTP_SERVER_H

#include "mcp_server.h"  // For event_dispatcher (used in legacy SSE mode)
#include "mcp_message.h"
#include "mcp_resource.h"
#include "mcp_tool.h"
#include "mcp_thread_pool.h"
#include "mcp_logger.h"

// Include the HTTP library
#include "httplib.h"

#include <string>
#include <map>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include <functional>
#include <chrono>
#include <condition_variable>
#include <future>
#include <atomic>
#include <optional>
#include <set>
#include <queue>
#include <random>

namespace mcp {

// ============================================================================
// Forward declarations
// ============================================================================
class streamable_http_session;

// ============================================================================
// Type aliases - inherited from mcp_server.h which is included above
// method_handler, tool_handler, notification_handler, auth_handler, session_cleanup_handler
// are already defined in mcp_server.h.
// ============================================================================

// ============================================================================
// SSE Writer: Writes SSE events to an HTTP response stream
// Corresponds to C# ISseEventStreamWriter
// ============================================================================
class sse_writer {
public:
    sse_writer() = default;
    ~sse_writer();

    /// Set the data sink for writing (from httplib chunked content provider)
    void set_sink(httplib::DataSink* sink);

    /// Write an SSE event to the stream
    /// @param event Event type (e.g., "message", "endpoint")
    /// @param data Event data
    /// @param id Optional event ID for resumability
    /// @return true if the write succeeded
    bool write_event(const std::string& event, const std::string& data, const std::string& id = "");

    /// Write raw data to the stream
    bool write_raw(const std::string& raw);

    /// Signal that no more events will be written (unblocks waiters)
    void complete();

    /// Check if the writer has been completed or closed
    bool is_completed() const;

    /// Wait for an event to become available and write it to the sink
    /// Returns false if the writer was closed/completed
    bool wait_and_flush(httplib::DataSink* sink, std::chrono::milliseconds timeout = std::chrono::milliseconds(30000));

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::string> pending_events_;
    httplib::DataSink* sink_ = nullptr;
    std::atomic<bool> completed_{false};
};

// ============================================================================
// Streamable HTTP Session
// Corresponds to C# StreamableHttpSession
// 
// Each session represents a persistent MCP connection with a unique session ID.
// Sessions track initialization state, active reference count, and idle time.
// ============================================================================
class streamable_http_session : public std::enable_shared_from_this<streamable_http_session> {
public:
    enum class state {
        uninitialized,
        started,
        disposed
    };

    explicit streamable_http_session(const std::string& session_id);
    ~streamable_http_session();

    /// Get the session ID
    const std::string& id() const { return session_id_; }

    /// Get session state
    state get_state() const;

    /// Mark session as started
    void start();

    /// Dispose the session (close all streams, release resources)
    void dispose();

    /// Check if session is disposed
    bool is_disposed() const;

    // --- Reference counting (C# AcquireReferenceAsync pattern) ---

    /// Acquire a reference (increments active request count, resets idle tracking)
    /// Returns false if session is disposed
    bool acquire_reference();

    /// Release a reference (decrements active request count, updates idle tracking)
    void release_reference();

    /// Get the number of active references
    int active_references() const;

    /// Check if the session is idle (no active references)
    bool is_idle() const;

    // --- Idle tracking ---

    /// Get the time of last activity
    std::chrono::steady_clock::time_point last_activity() const;

    /// Update the last activity timestamp
    void update_activity();

    // --- Initialization tracking ---

    /// Check if the session has been initialized (received notifications/initialized)
    bool is_initialized() const;

    /// Set the initialization state
    void set_initialized(bool initialized);

    // --- GET SSE stream (unsolicited server-to-client messages) ---
    // Corresponds to C# HandleGetRequestAsync / _httpSseWriter

    /// Try to start the GET SSE stream (only one GET per session allowed, matching C# TryStartGetRequest)
    /// @return shared_ptr to the sse_writer if successful, nullptr if already active
    std::shared_ptr<sse_writer> try_start_get_stream();

    /// Close the GET SSE stream
    void close_get_stream();

    /// Check if a GET SSE stream is currently active
    bool has_get_stream() const;

    /// Send an unsolicited message to the client via the GET SSE stream
    /// This is used for server-initiated notifications and sampling requests
    /// Corresponds to C# SendMessageAsync when no RelatedTransport is set
    bool send_unsolicited_message(const json& message);

    // --- Client info ---

    void set_client_info(const std::string& name, const std::string& version);
    std::string client_name() const;
    std::string client_version() const;

private:
    std::string session_id_;
    mutable std::mutex mutex_;
    std::atomic<state> state_{state::uninitialized};
    std::atomic<int> active_refs_{0};
    std::atomic<bool> initialized_{false};
    std::chrono::steady_clock::time_point last_activity_{std::chrono::steady_clock::now()};

    // GET SSE stream writer (one per session, for unsolicited messages)
    std::shared_ptr<sse_writer> get_stream_writer_;
    std::atomic<bool> get_stream_active_{false};

    // Client info
    std::string client_name_;
    std::string client_version_;
};

// ============================================================================
// RAII guard for session references (C# AcquireReferenceAsync return value)
// ============================================================================
class session_reference_guard {
public:
    explicit session_reference_guard(std::shared_ptr<streamable_http_session> session);
    ~session_reference_guard();

    session_reference_guard(const session_reference_guard&) = delete;
    session_reference_guard& operator=(const session_reference_guard&) = delete;
    session_reference_guard(session_reference_guard&& other) noexcept;
    session_reference_guard& operator=(session_reference_guard&& other) noexcept;

    bool is_valid() const { return session_ != nullptr; }

private:
    std::shared_ptr<streamable_http_session> session_;
};

// ============================================================================
// Streamable HTTP Server
// 
// Faithfully implements the Streamable HTTP transport from the C# MCP SDK.
// Maps three HTTP methods to a single endpoint path:
//   POST -> HandlePostRequestAsync (receives JSON-RPC, responds with SSE or JSON)
//   GET  -> HandleGetRequestAsync  (opens SSE stream for unsolicited messages)
//   DELETE -> HandleDeleteRequestAsync (client-initiated session termination)
// ============================================================================
class streamable_http_server {
public:
    /**
     * @struct configuration
     * @brief Configuration for the Streamable HTTP server
     * 
     * Corresponds to C# StreamableHttpServerOptions + McpServerOptions
     */
    struct configuration {
        /// Host to bind to
        std::string host{"localhost"};

        /// Port to listen on
        int port{8080};

        /// Server name
        std::string name{"MCP Streamable HTTP Server"};

        /// Server version
        std::string version{"0.0.1"};

        /// The endpoint path for all three HTTP methods (POST/GET/DELETE)
        /// Corresponds to C# MapMcp() route
        std::string endpoint{"/mcp"};

        /// Thread pool size
        unsigned int threadpool_size{std::thread::hardware_concurrency()};

        /// Whether the server operates in stateless mode
        /// In stateless mode:
        ///   - No session IDs are generated or tracked
        ///   - GET and DELETE endpoints are not available
        ///   - Each POST creates an ephemeral session for the duration of the request
        ///   - Server-to-client unsolicited messages are not supported
        /// Corresponds to C# StreamableHttpServerTransportOptions.Stateless
        bool stateless{false};

        /// Idle session timeout (default 2 hours, matching C# IdleTimeout)
        std::chrono::seconds idle_timeout{7200};

        /// Maximum number of idle sessions before pruning (C# MaxIdleSessionCount)
        size_t max_idle_sessions{10000};

        /// Idle check interval
        std::chrono::seconds idle_check_interval{5};

        /// Whether to enable legacy SSE endpoints (/sse + /message) for backward compatibility
        /// Corresponds to C# McpEndpointRouteBuilderExtensions.EnableLegacySse
        /// Cannot be used with stateless mode
        bool enable_legacy_sse{false};

        /// Legacy SSE endpoint path (only used if enable_legacy_sse is true)
        std::string legacy_sse_endpoint{"/sse"};

        /// Legacy message endpoint path (only used if enable_legacy_sse is true)
        std::string legacy_msg_endpoint{"/message"};

#ifdef MCP_SSL
        struct {
            std::optional<std::string> server_cert_path{std::nullopt};
            std::optional<std::string> server_private_key_path{std::nullopt};
        } ssl;
#endif
    };

    /**
     * @brief Constructor
     * @param conf Server configuration
     */
    explicit streamable_http_server(const configuration& conf);

    /**
     * @brief Destructor
     */
    ~streamable_http_server();

    // Non-copyable, non-movable
    streamable_http_server(const streamable_http_server&) = delete;
    streamable_http_server& operator=(const streamable_http_server&) = delete;

    // --- Server lifecycle ---

    /// Start the server
    /// @param blocking If true, blocks until the server stops
    /// @return true if the server started successfully
    bool start(bool blocking = true);

    /// Stop the server
    void stop();

    /// Check if the server is running
    bool is_running() const;

    // --- Server configuration ---

    /// Set server info (name, version)
    void set_server_info(const std::string& name, const std::string& version);

    /// Set server capabilities
    void set_capabilities(const json& capabilities);

    // --- Handler registration (same interface as existing mcp::server) ---

    /// Register a method handler
    void register_method(const std::string& method, method_handler handler);

    /// Register a notification handler
    void register_notification(const std::string& method, notification_handler handler);

    /// Register a resource
    void register_resource(const std::string& path, std::shared_ptr<resource> resource);

    /// Register a tool
    void register_tool(const tool& tool, tool_handler handler);

    /// Register a session cleanup handler
    void register_session_cleanup(const std::string& key, session_cleanup_handler handler);

    /// Get registered tools
    std::vector<tool> get_tools() const;

    /// Set authentication handler
    void set_auth_handler(auth_handler handler);

    // --- Server-to-client messaging ---

    /// Send a request/notification to a specific session via the GET SSE stream
    /// Corresponds to C# SendMessageAsync for unsolicited messages
    void send_request(const std::string& session_id, const request& req);

    /// Send a message to all connected sessions
    void broadcast(const request& req);

    /// Set static file mount point
    bool set_mount_point(const std::string& mount_point, const std::string& dir,
                         httplib::Headers headers = httplib::Headers());

private:
    // --- Configuration ---
    std::string host_;
    int port_;
    std::string name_;
    std::string version_;
    std::string endpoint_;
    json capabilities_;
    bool stateless_;
    std::chrono::seconds idle_timeout_;
    size_t max_idle_sessions_;
    std::chrono::seconds idle_check_interval_;
    bool enable_legacy_sse_;
    std::string legacy_sse_endpoint_;
    std::string legacy_msg_endpoint_;

    // --- HTTP server ---
    std::unique_ptr<httplib::Server> http_server_;
    std::unique_ptr<std::thread> server_thread_;
    std::atomic<bool> running_{false};

    // --- Thread pool ---
    thread_pool thread_pool_;

    // --- Session management (C# StatefulSessionManager) ---
    mutable std::mutex sessions_mutex_;
    std::map<std::string, std::shared_ptr<streamable_http_session>> sessions_;

    // --- Handler maps ---
    mutable std::mutex handlers_mutex_;
    std::map<std::string, method_handler> method_handlers_;
    std::map<std::string, notification_handler> notification_handlers_;
    std::map<std::string, std::shared_ptr<resource>> resources_;
    std::map<std::string, std::pair<tool, tool_handler>> tools_;
    auth_handler auth_handler_;
    std::map<std::string, session_cleanup_handler> session_cleanup_handlers_;

    // --- Idle session tracking (C# IdleTrackingBackgroundService) ---
    std::unique_ptr<std::thread> idle_tracking_thread_;
    std::mutex idle_mutex_;
    std::condition_variable idle_cv_;
    bool idle_thread_run_{false};

    // --- Legacy SSE support ---
    // When legacy SSE is enabled, we keep event_dispatchers for backward compat
    std::map<std::string, std::shared_ptr<event_dispatcher>> legacy_dispatchers_;
    std::map<std::string, std::unique_ptr<std::thread>> legacy_sse_threads_;

    // ========================================================================
    // HTTP endpoint handlers (correspond to C# StreamableHttpHandler methods)
    // ========================================================================

    /// POST handler - receives JSON-RPC requests/notifications
    /// Corresponds to C# HandlePostRequestAsync
    void handle_post(const httplib::Request& req, httplib::Response& res);

    /// GET handler - opens SSE stream for unsolicited server-to-client messages
    /// Corresponds to C# HandleGetRequestAsync
    void handle_get(const httplib::Request& req, httplib::Response& res);

    /// DELETE handler - client-initiated session termination
    /// Corresponds to C# HandleDeleteRequestAsync
    void handle_delete(const httplib::Request& req, httplib::Response& res);

    // --- Legacy SSE handlers (backward compat) ---
    void handle_legacy_sse(const httplib::Request& req, httplib::Response& res);
    void handle_legacy_message(const httplib::Request& req, httplib::Response& res);

    // ========================================================================
    // Validation helpers (correspond to C# validation in HandlePostRequestAsync)
    // ========================================================================

    /// Validate the Accept header for POST requests
    /// C# requires both "application/json" and "text/event-stream"
    bool validate_accept_header(const httplib::Request& req, httplib::Response& res);

    /// Validate the Mcp-Protocol-Version header
    /// Returns the version string if valid, empty string otherwise
    std::string validate_protocol_version_header(const httplib::Request& req, httplib::Response& res);

    /// Extract the Mcp-Session-Id from request headers
    std::string get_session_id_from_header(const httplib::Request& req);

    // ========================================================================
    // Session management (correspond to C# GetOrCreateSessionAsync, etc.)
    // ========================================================================

    /// Get or create a session based on the request
    /// Corresponds to C# GetOrCreateSessionAsync
    std::shared_ptr<streamable_http_session> get_or_create_session(
        const httplib::Request& req, httplib::Response& res,
        const json& message, bool& is_new_session);

    /// Create a new session
    std::shared_ptr<streamable_http_session> create_session();

    /// Find an existing session by ID
    std::shared_ptr<streamable_http_session> find_session(const std::string& session_id);

    /// Remove and dispose a session
    void remove_session(const std::string& session_id);

    /// Generate a cryptographically random session ID (C# RandomNumberGenerator)
    std::string generate_session_id() const;

    // ========================================================================
    // Request processing
    // ========================================================================

    /// Process a JSON-RPC request and return the response
    json process_request(const request& req, const std::string& session_id);

    /// Handle the initialize method
    json handle_initialize(const request& req, const std::string& session_id);

    // ========================================================================
    // Idle session management (C# IdleTrackingBackgroundService)
    // ========================================================================

    /// Background thread function for idle session tracking
    void idle_tracking_loop();

    /// Prune idle sessions (C# PruneIdleSessionsAsync)
    void prune_idle_sessions();

    /// Dispose all sessions (called on shutdown)
    void dispose_all_sessions();

    // ========================================================================
    // CORS
    // ========================================================================
    void setup_cors();
};

} // namespace mcp

#endif // MCP_STREAMABLE_HTTP_SERVER_H
