/**
 * NTONIX - High-Performance AI Inference Gateway
 *
 * A C++20 reverse proxy designed to optimize local LLM cluster infrastructure.
 */

#include "server/server.hpp"
#include "server/connection.hpp"

#include <boost/asio.hpp>
#include <spdlog/spdlog.h>

#include <iostream>

namespace asio = boost::asio;

int main(int argc, char* argv[]) {
    // Suppress unused parameter warnings
    (void)argc;
    (void)argv;

    // Set log level (DEBUG for development)
    spdlog::set_level(spdlog::level::debug);

    spdlog::info("NTONIX AI Inference Gateway v0.1.0");
    spdlog::info("Starting server...");

    try {
        // Configure server
        ntonix::server::ServerConfig config;
        config.port = 8080;
        config.thread_count = std::max(1u, std::thread::hardware_concurrency());
        config.bind_address = "0.0.0.0";

        spdlog::info("Configuration: port={}, threads={}, bind={}",
                    config.port, config.thread_count, config.bind_address);

        // Create and start server
        ntonix::server::Server server(config);

        // HTTP request handler using Boost.Beast
        auto request_handler = [](const ntonix::server::HttpRequest& req) -> ntonix::server::HttpResponse {
            using namespace ntonix::server;
            namespace http = boost::beast::http;

            spdlog::info("Request: {} {} Host={} Content-Type={}",
                        std::string(http::to_string(req.method)),
                        req.target,
                        req.host.empty() ? "(none)" : req.host,
                        req.content_type.empty() ? "(none)" : req.content_type);

            // Handle health check endpoint
            if (req.target == "/health" && req.method == http::verb::get) {
                return HttpResponse{
                    .status = http::status::ok,
                    .content_type = "application/json",
                    .body = R"({"status": "healthy"})"
                };
            }

            // Handle OpenAI-compatible chat completions endpoint
            if (req.target == "/v1/chat/completions" && req.method == http::verb::post) {
                // Check Content-Type for JSON
                if (req.content_type.find("application/json") == std::string::npos) {
                    return HttpResponse{
                        .status = http::status::unsupported_media_type,
                        .content_type = "application/json",
                        .body = R"({"error": "Content-Type must be application/json"})"
                    };
                }

                // Log the request body (for development)
                spdlog::debug("Request body: {}", req.body);

                // For now, return a mock response
                // Real implementation will forward to LLM backend
                return HttpResponse{
                    .status = http::status::ok,
                    .content_type = "application/json",
                    .body = R"({
  "id": "chatcmpl-mock",
  "object": "chat.completion",
  "created": 1234567890,
  "model": "mock-model",
  "choices": [{
    "index": 0,
    "message": {
      "role": "assistant",
      "content": "This is a mock response from NTONIX gateway."
    },
    "finish_reason": "stop"
  }],
  "usage": {
    "prompt_tokens": 10,
    "completion_tokens": 20,
    "total_tokens": 30
  }
})"
                };
            }

            // Handle root path - gateway info
            if (req.target == "/" && req.method == http::verb::get) {
                return HttpResponse{
                    .status = http::status::ok,
                    .content_type = "application/json",
                    .body = R"({
  "name": "NTONIX",
  "version": "0.1.0",
  "description": "High-Performance AI Inference Gateway",
  "endpoints": {
    "health": "/health",
    "chat_completions": "/v1/chat/completions"
  }
})"
                };
            }

            // 404 for unknown endpoints
            return HttpResponse{
                .status = http::status::not_found,
                .content_type = "application/json",
                .body = R"({"error": "Not found"})"
            };
        };

        // Connection handler - wraps each connection with HTTP parsing
        server.start([request_handler](asio::ip::tcp::socket socket) {
            ntonix::server::handle_connection(std::move(socket), request_handler);
        });

        spdlog::info("Server started successfully");
        spdlog::info("Press Ctrl+C to stop");

        // Wait for shutdown (blocks until signal received)
        server.wait();

        spdlog::info("Server stopped gracefully");
        return 0;

    } catch (const std::exception& e) {
        spdlog::error("Fatal error: {}", e.what());
        return 1;
    }
}
