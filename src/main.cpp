/**
 * NTONIX - High-Performance AI Inference Gateway
 *
 * A C++20 reverse proxy designed to optimize local LLM cluster infrastructure.
 */

#include "config/config.hpp"
#include "server/server.hpp"
#include "server/connection.hpp"
#include "balancer/health_checker.hpp"
#include "balancer/load_balancer.hpp"
#include "proxy/connection_pool.hpp"
#include "proxy/forwarder.hpp"
#include "cache/lru_cache.hpp"
#include "cache/cache_key.hpp"

#include <boost/asio.hpp>
#include <spdlog/spdlog.h>

#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>

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

        // Create load balancer with health checker integration
        auto load_balancer = std::make_shared<ntonix::balancer::LoadBalancer>(health_checker);
        load_balancer->set_backends(config.backends);
        spdlog::info("Load balancer configured with {} backends", config.backends.size());

        // Create connection pool manager for backend connections
        ntonix::proxy::ConnectionPoolConfig pool_config;
        pool_config.pool_size_per_backend = 10;    // Max 10 connections per backend
        pool_config.idle_timeout = std::chrono::seconds(60);
        pool_config.cleanup_interval = std::chrono::seconds(30);
        pool_config.enable_keep_alive = true;

        auto connection_pool = std::make_shared<ntonix::proxy::ConnectionPoolManager>(
            server.get_io_context(), pool_config);
        connection_pool->set_backends(config.backends);
        spdlog::info("Connection pool manager configured (pool_size={} per backend)",
                    pool_config.pool_size_per_backend);

        // Create request forwarder for proxying to backends
        ntonix::proxy::ForwarderConfig forwarder_config;
        forwarder_config.request_timeout = std::chrono::seconds(60);  // LLM requests can be slow
        forwarder_config.connect_timeout = std::chrono::seconds(5);
        forwarder_config.add_forwarded_headers = true;
        forwarder_config.generate_request_id = true;

        auto forwarder = std::make_shared<ntonix::proxy::Forwarder>(
            server.get_io_context(), connection_pool, forwarder_config);
        spdlog::info("Request forwarder configured (timeout={}s)",
                    forwarder_config.request_timeout.count());

        // Create LRU cache for response caching
        ntonix::cache::LruCacheConfig cache_config;
        cache_config.max_size_bytes = config.cache.max_size_mb * 1024 * 1024;
        cache_config.ttl = std::chrono::seconds(config.cache.ttl_seconds);
        cache_config.enabled = config.cache.enabled;

        auto response_cache = std::make_shared<ntonix::cache::LruCache>(cache_config);
        if (config.cache.enabled) {
            spdlog::info("Response cache configured: max_size={}MB, ttl={}s",
                        config.cache.max_size_mb, config.cache.ttl_seconds);
        } else {
            spdlog::info("Response cache: disabled");
        }

        // Register SIGHUP handler for config reload (Unix only, handled in server via signal_set)
        config_manager.on_reload([health_checker, load_balancer, connection_pool](const std::vector<ntonix::config::BackendConfig>& backends) {
            spdlog::info("Backend configuration reloaded with {} backends", backends.size());
            for (const auto& backend : backends) {
                spdlog::info("  - {}:{} (weight={})", backend.host, backend.port, backend.weight);
            }
            // Update health checker, load balancer, and connection pool with new backend list
            health_checker->set_backends(backends);
            load_balancer->set_backends(backends);
            connection_pool->set_backends(backends);
        });

        // Streaming request handler - handles SSE streaming responses
        auto streaming_handler = [load_balancer, forwarder](
            const ntonix::server::HttpRequest& req,
            boost::beast::tcp_stream& client_stream) -> bool {

            using namespace ntonix::server;
            namespace http = boost::beast::http;

            // Only handle streaming for POST /v1/chat/completions with stream=true
            if (req.target != "/v1/chat/completions" || req.method != http::verb::post) {
                return false;  // Let normal handler process this
            }

            // Check if this is a streaming request
            if (!ntonix::proxy::Forwarder::is_streaming_request(req)) {
                return false;  // Not a streaming request, use normal handler
            }

            spdlog::info("Streaming request: {} {} Client={}",
                        std::string(http::to_string(req.method)),
                        req.target,
                        req.client_ip.empty() ? "(unknown)" : req.client_ip);

            // Check Content-Type for JSON
            if (req.content_type.find("application/json") == std::string::npos) {
                // Return error via stream (we need to write response ourselves)
                http::response<http::string_body> error_response{http::status::unsupported_media_type, 11};
                error_response.set(http::field::server, "NTONIX/0.1.0");
                error_response.set(http::field::content_type, "application/json");
                error_response.body() = R"({"error": "Content-Type must be application/json"})";
                error_response.prepare_payload();
                boost::beast::error_code ec;
                http::write(client_stream, error_response, ec);
                return true;  // We handled it
            }

            // Select backend using load balancer
            auto backend_selection = load_balancer->select_backend();
            if (!backend_selection) {
                spdlog::warn("No healthy backends available for streaming request");
                http::response<http::string_body> error_response{http::status::service_unavailable, 11};
                error_response.set(http::field::server, "NTONIX/0.1.0");
                error_response.set(http::field::content_type, "application/json");
                error_response.body() = R"({"error": "No healthy backends available"})";
                error_response.prepare_payload();
                boost::beast::error_code ec;
                http::write(client_stream, error_response, ec);
                return true;
            }

            const auto& backend = backend_selection->backend;
            spdlog::info("Load balancer selected backend {}:{} for streaming (index={})",
                        backend.host, backend.port, backend_selection->index);

            // Forward with streaming support
            auto result = forwarder->forward_with_streaming(req, backend, client_stream, req.client_ip);

            if (result.is_streaming) {
                spdlog::info("Streaming complete: {} bytes forwarded from {}:{} in {}ms",
                            result.stream_result.bytes_forwarded,
                            result.backend_host, result.backend_port,
                            result.latency.count());
            } else {
                // Backend returned non-streaming response, send it to client
                http::response<http::string_body> response{result.response.status, 11};
                response.set(http::field::server, "NTONIX/0.1.0");
                response.set(http::field::content_type, result.response.content_type);
                for (const auto& [name, value] : result.response.headers) {
                    response.set(name, value);
                }
                response.body() = result.response.body;
                response.prepare_payload();
                boost::beast::error_code ec;
                http::write(client_stream, response, ec);
            }

            if (!result.success) {
                spdlog::warn("Streaming forward failed: {}", result.error_message);
            }

            return true;  // We handled the request
        };

        // HTTP request handler using Boost.Beast (non-streaming requests)
        auto request_handler = [load_balancer, forwarder, response_cache](const ntonix::server::HttpRequest& req) -> ntonix::server::HttpResponse {
            using namespace ntonix::server;
            namespace http = boost::beast::http;

            spdlog::info("Request: {} {} Host={} Content-Type={} Client={}",
                        std::string(http::to_string(req.method)),
                        req.target,
                        req.host.empty() ? "(none)" : req.host,
                        req.content_type.empty() ? "(none)" : req.content_type,
                        req.client_ip.empty() ? "(unknown)" : req.client_ip);

            // Handle health check endpoint
            if (req.target == "/health" && req.method == http::verb::get) {
                return HttpResponse{
                    .status = http::status::ok,
                    .content_type = "application/json",
                    .body = R"({"status": "healthy"})"
                };
            }

            // Handle cache statistics endpoint
            if (req.target == "/cache/stats" && req.method == http::verb::get) {
                auto stats = response_cache->get_stats();
                std::ostringstream json;
                json << "{\n"
                     << "  \"enabled\": " << (response_cache->is_enabled() ? "true" : "false") << ",\n"
                     << "  \"hits\": " << stats.hits << ",\n"
                     << "  \"misses\": " << stats.misses << ",\n"
                     << "  \"hit_rate\": " << std::fixed << std::setprecision(4) << stats.hit_rate() << ",\n"
                     << "  \"evictions\": " << stats.evictions << ",\n"
                     << "  \"expired\": " << stats.expired << ",\n"
                     << "  \"entries\": " << stats.entries << ",\n"
                     << "  \"size_bytes\": " << stats.size_bytes << ",\n"
                     << "  \"max_size_bytes\": " << stats.max_size_bytes << "\n"
                     << "}";
                return HttpResponse{
                    .status = http::status::ok,
                    .content_type = "application/json",
                    .body = json.str()
                };
            }

            // Handle OpenAI-compatible chat completions endpoint (non-streaming only)
            // Note: Streaming requests are handled by streaming_handler above
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

                // Check for cache bypass via Cache-Control header
                std::string cache_control;
                auto it = req.raw_request.find(http::field::cache_control);
                if (it != req.raw_request.end()) {
                    cache_control = std::string(it->value());
                }
                bool bypass_cache = ntonix::cache::should_bypass_cache(cache_control);

                // Generate cache key from request
                auto cache_key = ntonix::cache::generate_cache_key(
                    std::string(http::to_string(req.method)),
                    req.target,
                    req.body
                );

                // Try cache lookup (unless bypass requested)
                if (!bypass_cache && response_cache->is_enabled()) {
                    auto cached = response_cache->get(cache_key);
                    if (cached) {
                        spdlog::info("Cache HIT: key={}", cache_key.to_string());
                        return HttpResponse{
                            .status = http::status::ok,
                            .content_type = cached->content_type,
                            .body = cached->body,
                            .headers = {{"X-Cache", "HIT"}}
                        };
                    }
                    spdlog::debug("Cache MISS: key={}", cache_key.to_string());
                }

                // Select backend using load balancer
                auto backend_selection = load_balancer->select_backend();
                if (!backend_selection) {
                    spdlog::warn("No healthy backends available - returning 503");
                    return HttpResponse{
                        .status = http::status::service_unavailable,
                        .content_type = "application/json",
                        .body = R"({"error": "No healthy backends available"})"
                    };
                }

                const auto& backend = backend_selection->backend;
                spdlog::info("Load balancer selected backend {}:{} (index={})",
                            backend.host, backend.port, backend_selection->index);

                // Forward the request to the selected backend (non-streaming)
                auto result = forwarder->forward(req, backend, req.client_ip);

                spdlog::info("Backend response: {} from {}:{} in {}ms",
                            static_cast<int>(result.response.status),
                            result.backend_host, result.backend_port,
                            result.latency.count());

                if (!result.success) {
                    spdlog::warn("Forward failed: {}", result.error_message);
                }

                // Cache successful responses (2xx status codes only)
                if (result.success && response_cache->is_enabled() && !bypass_cache &&
                    static_cast<int>(result.response.status) >= 200 &&
                    static_cast<int>(result.response.status) < 300) {
                    response_cache->put(cache_key, result.response.body, result.response.content_type);
                    spdlog::debug("Cached response: key={}, size={}", cache_key.to_string(), result.response.body.size());
                }

                // Add cache header to response
                result.response.headers.push_back({"X-Cache", "MISS"});
                return result.response;
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
    "cache_stats": "/cache/stats",
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
            [request_handler, streaming_handler](asio::ip::tcp::socket socket) {
                ntonix::server::handle_connection(std::move(socket), request_handler, streaming_handler);
            },
            [&config_manager]() {
                config_manager.reload();
            }
        );

        // Start health checker and connection pool after server starts (uses server's io_context)
        if (!config.backends.empty()) {
            health_checker->start();
            spdlog::info("Health checker started for {} backends", config.backends.size());
            connection_pool->start_cleanup();
            spdlog::info("Connection pool cleanup timer started");
        }

        spdlog::info("Server started successfully");
        spdlog::info("Press Ctrl+C to stop");

        // Wait for shutdown (blocks until signal received)
        server.wait();

        // Stop health checker and connection pool
        health_checker->stop();
        connection_pool->stop_cleanup();

        spdlog::info("Server stopped gracefully");
        return 0;

    } catch (const std::exception& e) {
        spdlog::error("Fatal error: {}", e.what());
        return 1;
    }
}
