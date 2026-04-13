/**
 * @file streamable_http_server_example.cpp
 * @brief Example demonstrating the Streamable HTTP transport for MCP
 * 
 * This example shows how to create an MCP server using the Streamable HTTP transport,
 * which is the modern MCP transport ported from the C# SDK.
 * 
 * Key differences from the old SSE transport:
 * - Single endpoint (/mcp) handles POST, GET, and DELETE
 * - POST responses flow back on the same HTTP response as SSE events
 * - GET opens an SSE stream for unsolicited server-to-client messages
 * - DELETE allows client-initiated session termination
 * - Session management via Mcp-Session-Id header
 * - Optional stateless mode (no session tracking)
 * - Optional legacy SSE compatibility
 */

#include "mcp_streamable_http_server.h"
#include "mcp_tool.h"
#include "mcp_resource.h"

#include <iostream>
#include <chrono>
#include <ctime>
#include <thread>
#include <filesystem>
#include <algorithm>

// Tool handler for getting current time
mcp::json get_time_handler(const mcp::json& params, const std::string& /* session_id */) {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    
    std::string time_str = std::ctime(&time_t_now);
    if (!time_str.empty() && time_str[time_str.length() - 1] == '\n') {
        time_str.erase(time_str.length() - 1);
    }
    
    return {
        {
            {"type", "text"},
            {"text", time_str}
        }
    };
}

// Echo tool handler
mcp::json echo_handler(const mcp::json& params, const std::string& /* session_id */) {
    mcp::json result = params;
    
    if (params.contains("text")) {
        std::string text = params["text"];
        
        if (params.contains("uppercase") && params["uppercase"].get<bool>()) {
            std::transform(text.begin(), text.end(), text.begin(), ::toupper);
            result["text"] = text;
        }
        
        if (params.contains("reverse") && params["reverse"].get<bool>()) {
            std::reverse(text.begin(), text.end());
            result["text"] = text;
        }
    }
    
    return {
        {
            {"type", "text"},
            {"text", result["text"].get<std::string>()}
        }
    };
}

// Calculator tool handler
mcp::json calculator_handler(const mcp::json& params, const std::string& /* session_id */) {
    if (!params.contains("operation")) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "Missing 'operation' parameter");
    }
    
    std::string operation = params["operation"];
    double result = 0.0;
    
    if (operation == "add") {
        if (!params.contains("a") || !params.contains("b")) {
            throw mcp::mcp_exception(mcp::error_code::invalid_params, "Missing 'a' or 'b' parameter");
        }
        result = params["a"].get<double>() + params["b"].get<double>();
    } else if (operation == "subtract") {
        if (!params.contains("a") || !params.contains("b")) {
            throw mcp::mcp_exception(mcp::error_code::invalid_params, "Missing 'a' or 'b' parameter");
        }
        result = params["a"].get<double>() - params["b"].get<double>();
    } else if (operation == "multiply") {
        if (!params.contains("a") || !params.contains("b")) {
            throw mcp::mcp_exception(mcp::error_code::invalid_params, "Missing 'a' or 'b' parameter");
        }
        result = params["a"].get<double>() * params["b"].get<double>();
    } else if (operation == "divide") {
        if (!params.contains("a") || !params.contains("b")) {
            throw mcp::mcp_exception(mcp::error_code::invalid_params, "Missing 'a' or 'b' parameter");
        }
        if (params["b"].get<double>() == 0.0) {
            throw mcp::mcp_exception(mcp::error_code::invalid_params, "Division by zero not allowed");
        }
        result = params["a"].get<double>() / params["b"].get<double>();
    } else {
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "Unknown operation: " + operation);
    }
    
    return {
        {
            {"type", "text"},
            {"text", std::to_string(result)}
        }
    };
}

// Hello tool handler
mcp::json hello_handler(const mcp::json& params, const std::string& /* session_id */) {
    std::string name = params.contains("name") ? params["name"].get<std::string>() : "World";
    return {
        {
            {"type", "text"},
            {"text", "Hello, " + name + "!"}
        }
    };
}

int main() {
    // ========================================================================
    // Configure the Streamable HTTP server
    // ========================================================================
    mcp::streamable_http_server::configuration conf;
    conf.host = "localhost";
    conf.port = 8888;
    conf.name = "ExampleStreamableServer";
    conf.version = "1.0.0";
    
    // The single endpoint path for POST/GET/DELETE (Streamable HTTP transport)
    conf.endpoint = "/mcp";

    // Stateless mode: set to true if you don't need server-to-client unsolicited messages
    // In stateless mode, each POST is independent (no session tracking)
    conf.stateless = false;

    // Idle session settings (matching C# defaults)
    conf.idle_timeout = std::chrono::seconds(7200);      // 2 hours
    conf.max_idle_sessions = 10000;
    conf.idle_check_interval = std::chrono::seconds(5);

    // Optional: enable legacy SSE endpoints for backward compatibility
    // This adds /sse (GET) and /message (POST) endpoints alongside /mcp
    conf.enable_legacy_sse = true;
    conf.legacy_sse_endpoint = "/sse";
    conf.legacy_msg_endpoint = "/message";

    // ========================================================================
    // Create and configure the server
    // ========================================================================
    mcp::streamable_http_server server(conf);

    // Set capabilities
    mcp::json capabilities = {
        {"tools", mcp::json::object()}
    };
    server.set_capabilities(capabilities);

    // ========================================================================
    // Register tools (same API as the old mcp::server)
    // ========================================================================
    mcp::tool time_tool = mcp::tool_builder("get_time")
        .with_description("Get current time")
        .build();
    
    mcp::tool echo_tool = mcp::tool_builder("echo")
        .with_description("Echo input with optional transformations")
        .with_string_param("text", "Text to echo")
        .with_boolean_param("uppercase", "Convert to uppercase", false)
        .with_boolean_param("reverse", "Reverse the text", false)
        .build();
    
    mcp::tool calc_tool = mcp::tool_builder("calculator")
        .with_description("Perform basic calculations")
        .with_string_param("operation", "Operation to perform (add, subtract, multiply, divide)")
        .with_number_param("a", "First operand")
        .with_number_param("b", "Second operand")
        .build();

    mcp::tool hello_tool = mcp::tool_builder("hello")
        .with_description("Say hello")
        .with_string_param("name", "Name to say hello to", "World")
        .build();
    
    server.register_tool(time_tool, get_time_handler);
    server.register_tool(echo_tool, echo_handler);
    server.register_tool(calc_tool, calculator_handler);
    server.register_tool(hello_tool, hello_handler);

    // ========================================================================
    // Start the server
    // ========================================================================
    std::cout << "========================================" << std::endl;
    std::cout << "MCP Streamable HTTP Server" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    std::cout << "Listening on: http://" << conf.host << ":" << conf.port << std::endl;
    std::cout << std::endl;
    std::cout << "Streamable HTTP endpoint:" << std::endl;
    std::cout << "  POST   " << conf.endpoint << "  (JSON-RPC requests)" << std::endl;
    std::cout << "  GET    " << conf.endpoint << "  (SSE stream for unsolicited messages)" << std::endl;
    std::cout << "  DELETE " << conf.endpoint << "  (Session termination)" << std::endl;
    std::cout << std::endl;
    if (conf.enable_legacy_sse) {
        std::cout << "Legacy SSE endpoints (backward compatible):" << std::endl;
        std::cout << "  GET    " << conf.legacy_sse_endpoint << "     (SSE connection)" << std::endl;
        std::cout << "  POST   " << conf.legacy_msg_endpoint << "  (JSON-RPC messages)" << std::endl;
        std::cout << std::endl;
    }
    std::cout << "Mode: " << (conf.stateless ? "Stateless" : "Stateful") << std::endl;
    std::cout << std::endl;
    std::cout << "Press Ctrl+C to stop the server" << std::endl;
    std::cout << "========================================" << std::endl;

    server.start(true);  // Blocking mode

    return 0;
}
