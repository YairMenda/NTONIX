/**
 * NTONIX - High-Performance AI Inference Gateway
 *
 * A C++20 reverse proxy designed to optimize local LLM cluster infrastructure.
 */

#include "config/config.hpp"
#include "server/server.hpp"
#include "server/connection.hpp"
#include "balancer/health_checker.hpp"

#include <boost/asio.hpp>
#include <spdlog/spdlog.h>

#include <iostream>
#include <memory>

namespace asio = boost::asio;

int main(int argc, char* argv[]) {
    // Set log level (DEBUG for development)
    spdlog::set_level(spdlog::level::debug);

    spdlog::info("NTONIX AI Inference Gateway v0.1.0");

    try {
        // Load configuration
        ntonix::config::ConfigManager config_manager;
        if (!config_manager.load(argc, argv)) {
            // --help was requested
            return 0;
        }

        auto config = config_manager.get_config();

        // Configure server from loaded config
        ntonix::server::ServerConfig server_config;
        server_config.port = config.server.port;
        server_config.thread_count = config.server.threads > 0
            ? config.server.threads
            : std::max(1u, std::thread::hardware_concurrency());
        server_config.bind_address = config.server.bind_address;

        spdlog::info("Configuration: port={}, threads={}, bind={}",
                    server_config.port, server_config.thread_count, server_config.bind_address);

        // Log backend configuration
        if (config.backends.empty()) {
            spdlog::warn("No backends configured - proxy will return 503 for all forwarding requests");
        } else {
            spdlog::info("Backends configured:");
            for (const auto& backend : config.backends) {
                spdlog::info("  - {}:{} (weight={})", backend.host, backend.port, backend.weight);
            }
        }

        // Log cache configuration
        if (config.cache.enabled) {
            spdlog::info("Cache: enabled, max_size={}MB, ttl={}s",
                        config.cache.max_size_mb, config.cache.ttl_seconds);
        } else {
            spdlog::info("Cache: disabled");
        }

        // Create and start server
        ntonix::server::Server server(server_config);

        // Create health checker for backend monitoring
        ntonix::balancer::HealthCheckConfig health_config;
        health_config.interval = std::chrono::milliseconds(5000);  // Check every 5 seconds
        health_config.timeout = std::chrono::milliseconds(2000);   // 2 second timeout
        health_config.unhealthy_threshold = 3;                      // Mark unhealthy after 3 failures
        health_config.healthy_threshold = 2;                        // Mark healthy after 2 successes

        auto health_checker = std::make_shared<ntonix::balancer::HealthChecker>(
            server.get_io_context(), health_config);

        // Set initial backends
        health_checker->set_backends(config.backends);

        // Register state change callback for logging
        health_checker->on_state_change([](
            const ntonix::config::BackendConfig& backend,
            ntonix::balancer::BackendState old_state,
            ntonix::balancer::BackendState new_state) {
            spdlog::info("Backend {}:{} health state: {} -> {}",
                         backend.host, backend.port,
                         ntonix::balancer::to_string(old_state),
                         ntonix::balancer::to_string(new_state));
        });

        // Register SIGHUP handler for config reload (Unix only, handled in server via signal_set)
        config_manager.on_reload([health_checker](const std::vector<ntonix::config::BackendConfig>& backends) {
            spdlog::info("Backend configuration reloaded with {} backends", backends.size());
            for (const auto& backend : backends) {
                spdlog::info("  - {}:{} (weight={})", backend.host, backend.port, backend.weight);
            }
            // Update health checker with new backend list
            health_checker->set_backends(backends);
        });

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
        // Reload handler - called on SIGHUP to reload configuration
        server.start(
            [request_handler](asio::ip::tcp::socket socket) {
                ntonix::server::handle_connection(std::move(socket), request_handler);
            },
            [&config_manager]() {
                config_manager.reload();
            }
        );

        // Start health checker after server starts (uses server's io_context)
        if (!config.backends.empty()) {
            health_checker->start();
            spdlog::info("Health checker started for {} backends", config.backends.size());
        }

        spdlog::info("Server started successfully");
        spdlog::info("Press Ctrl+C to stop");

        // Wait for shutdown (blocks until signal received)
        server.wait();

        // Stop health checker
        health_checker->stop();

        spdlog::info("Server stopped gracefully");
        return 0;

    } catch (const std::exception& e) {
        spdlog::error("Fatal error: {}", e.what());
        return 1;
    }
}
