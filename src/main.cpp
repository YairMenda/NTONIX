/**
 * NTONIX - High-Performance AI Inference Gateway
 *
 * A C++20 reverse proxy designed to optimize local LLM cluster infrastructure.
 */

#include "config/config.hpp"
#include "server/server.hpp"
#include "server/connection.hpp"
#include "server/ssl_server.hpp"
#include "server/ssl_connection.hpp"
#include "balancer/health_checker.hpp"
#include "balancer/load_balancer.hpp"
#include "proxy/connection_pool.hpp"
#include "proxy/forwarder.hpp"
#include "cache/lru_cache.hpp"
#include "cache/cache_key.hpp"
#include "util/logger.hpp"

#include <boost/asio.hpp>
#include <boost/beast/ssl.hpp>
#include <spdlog/spdlog.h>

#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>

namespace asio = boost::asio;

// Helper to convert config log settings to logger config
ntonix::util::LogConfig make_log_config(const ntonix::config::LogSettings& settings) {
    ntonix::util::LogConfig log_config;
    log_config.file_path = settings.file;
    log_config.max_file_size_mb = settings.max_file_size_mb;
    log_config.max_files = settings.max_files;
    log_config.enable_console = settings.enable_console;
    log_config.enable_colors = settings.enable_colors;

    // Parse log level
    if (auto level = ntonix::util::Logger::parse_level(settings.level)) {
        log_config.level = *level;
    } else {
        log_config.level = ntonix::util::LogLevel::Info;  // Default to info
    }

    return log_config;
}

int main(int argc, char* argv[]) {
    // Initialize with default logging until config is loaded
    ntonix::util::Logger::init_default();
    auto& logger = ntonix::util::Logger::instance();

    NTONIX_LOG_INFO("server", "NTONIX AI Inference Gateway v0.1.0");

    try {
        // Load configuration
        ntonix::config::ConfigManager config_manager;
        if (!config_manager.load(argc, argv)) {
            // --help was requested
            return 0;
        }

        auto config = config_manager.get_config();

        // Reconfigure logger with loaded settings
        auto log_config = make_log_config(config.logging);
        logger.set_level(log_config.level);
        NTONIX_LOG_INFO("config", "Log level set to: {}", ntonix::util::Logger::level_to_string(log_config.level));

        // Configure server from loaded config
        ntonix::server::ServerConfig server_config;
        server_config.port = config.server.port;
        server_config.thread_count = config.server.threads > 0
            ? config.server.threads
            : std::max(1u, std::thread::hardware_concurrency());
        server_config.bind_address = config.server.bind_address;

        NTONIX_LOG_INFO("config", "Configuration: port={}, threads={}, bind={}",
                    server_config.port, server_config.thread_count, server_config.bind_address);

        // Log backend configuration
        if (config.backends.empty()) {
            NTONIX_LOG_WARN("config", "No backends configured - proxy will return 503 for all forwarding requests");
        } else {
            NTONIX_LOG_INFO("config", "Backends configured:");
            for (const auto& backend : config.backends) {
                NTONIX_LOG_INFO("config", "  - {}:{} (weight={})", backend.host, backend.port, backend.weight);
            }
        }

        // Log cache configuration
        if (config.cache.enabled) {
            NTONIX_LOG_INFO("config", "Cache: enabled, max_size={}MB, ttl={}s",
                        config.cache.max_size_mb, config.cache.ttl_seconds);
        } else {
            NTONIX_LOG_INFO("config", "Cache: disabled");
        }

        // Log SSL configuration
        if (config.ssl.enabled) {
            NTONIX_LOG_INFO("config", "SSL: enabled, port={}, cert={}, key={}",
                        config.server.ssl_port, config.ssl.cert_file, config.ssl.key_file);
        } else {
            NTONIX_LOG_INFO("config", "SSL: disabled");
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
            NTONIX_LOG_INFO("health", "Backend {}:{} health state: {} -> {}",
                         backend.host, backend.port,
                         ntonix::balancer::to_string(old_state),
                         ntonix::balancer::to_string(new_state));
        });

        // Create load balancer with health checker integration
        auto load_balancer = std::make_shared<ntonix::balancer::LoadBalancer>(health_checker);
        load_balancer->set_backends(config.backends);
        NTONIX_LOG_INFO("balancer", "Load balancer configured with {} backends", config.backends.size());

        // Create connection pool manager for backend connections
        ntonix::proxy::ConnectionPoolConfig pool_config;
        pool_config.pool_size_per_backend = 10;    // Max 10 connections per backend
        pool_config.idle_timeout = std::chrono::seconds(60);
        pool_config.cleanup_interval = std::chrono::seconds(30);
        pool_config.enable_keep_alive = true;

        auto connection_pool = std::make_shared<ntonix::proxy::ConnectionPoolManager>(
            server.get_io_context(), pool_config);
        connection_pool->set_backends(config.backends);
        NTONIX_LOG_INFO("pool", "Connection pool manager configured (pool_size={} per backend)",
                    pool_config.pool_size_per_backend);

        // Create request forwarder for proxying to backends
        ntonix::proxy::ForwarderConfig forwarder_config;
        forwarder_config.request_timeout = std::chrono::seconds(60);  // LLM requests can be slow
        forwarder_config.connect_timeout = std::chrono::seconds(5);
        forwarder_config.add_forwarded_headers = true;
        forwarder_config.generate_request_id = true;

        auto forwarder = std::make_shared<ntonix::proxy::Forwarder>(
            server.get_io_context(), connection_pool, forwarder_config);
        NTONIX_LOG_INFO("proxy", "Request forwarder configured (timeout={}s)",
                    forwarder_config.request_timeout.count());

        // Create LRU cache for response caching
        ntonix::cache::LruCacheConfig cache_config;
        cache_config.max_size_bytes = config.cache.max_size_mb * 1024 * 1024;
        cache_config.ttl = std::chrono::seconds(config.cache.ttl_seconds);
        cache_config.enabled = config.cache.enabled;

        auto response_cache = std::make_shared<ntonix::cache::LruCache>(cache_config);
        if (config.cache.enabled) {
            NTONIX_LOG_INFO("cache", "Response cache configured: max_size={}MB, ttl={}s",
                        config.cache.max_size_mb, config.cache.ttl_seconds);
        } else {
            NTONIX_LOG_INFO("cache", "Response cache: disabled");
        }

        // Register SIGHUP handler for config reload (Unix only, handled in server via signal_set)
        config_manager.on_reload([health_checker, load_balancer, connection_pool](const std::vector<ntonix::config::BackendConfig>& backends) {
            NTONIX_LOG_INFO("config", "Backend configuration reloaded with {} backends", backends.size());
            for (const auto& backend : backends) {
                NTONIX_LOG_INFO("config", "  - {}:{} (weight={})", backend.host, backend.port, backend.weight);
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

            NTONIX_LOG_INFO("proxy", "Streaming request: {} {} Client={}",
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
                NTONIX_LOG_WARN("balancer", "No healthy backends available for streaming request");
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
            NTONIX_LOG_DEBUG("balancer", "Load balancer selected backend {}:{} for streaming (index={})",
                        backend.host, backend.port, backend_selection->index);

            // Forward with streaming support
            auto result = forwarder->forward_with_streaming(req, backend, client_stream, req.client_ip);

            if (result.is_streaming) {
                // Log access for streaming request
                ntonix::util::AccessLogEntry access_entry;
                access_entry.request_id = req.x_request_id;
                access_entry.client_ip = req.client_ip;
                access_entry.method = std::string(http::to_string(req.method));
                access_entry.path = req.target;
                access_entry.status_code = 200;  // Streaming always starts with 200
                access_entry.response_size = result.stream_result.bytes_forwarded;
                access_entry.latency = result.latency;
                access_entry.cache_hit = false;
                access_entry.backend_host = result.backend_host;
                access_entry.backend_port = result.backend_port;
                ntonix::util::Logger::instance().access(access_entry);

                NTONIX_LOG_DEBUG("proxy", "Streaming complete: {} bytes forwarded from {}:{} in {}ms",
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
                NTONIX_LOG_WARN("proxy", "Streaming forward failed: {}", result.error_message);
            }

            return true;  // We handled the request
        };

        // SSL Streaming request handler
        // Note: For SSL connections, streaming is not yet supported. Streaming requests
        // will be processed by the normal request handler as non-streaming requests.
        // This produces correct output but without the streaming behavior.
        ntonix::server::SslStreamingRequestHandler ssl_streaming_handler = nullptr;

        // HTTP request handler using Boost.Beast (non-streaming requests)
        auto request_handler = [load_balancer, forwarder, response_cache](const ntonix::server::HttpRequest& req) -> ntonix::server::HttpResponse {
            using namespace ntonix::server;
            namespace http = boost::beast::http;

            // Start timing for access log
            auto start_time = std::chrono::steady_clock::now();

            // Create request context for X-Request-ID propagation
            ntonix::util::RequestContext request_ctx(req.x_request_id);
            std::string request_id = request_ctx.id();

            NTONIX_LOG_DEBUG("server", "Request: {} {} Host={} Content-Type={} Client={} RequestID={}",
                        std::string(http::to_string(req.method)),
                        req.target,
                        req.host.empty() ? "(none)" : req.host,
                        req.content_type.empty() ? "(none)" : req.content_type,
                        req.client_ip.empty() ? "(unknown)" : req.client_ip,
                        request_id);

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
                NTONIX_LOG_TRACE("proxy", "Request body: {}", req.body);

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
                        NTONIX_LOG_DEBUG("cache", "Cache HIT: key={}", cache_key.to_string());

                        // Calculate latency and log access
                        auto end_time = std::chrono::steady_clock::now();
                        auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

                        ntonix::util::AccessLogEntry access_entry;
                        access_entry.request_id = request_id;
                        access_entry.client_ip = req.client_ip;
                        access_entry.method = std::string(http::to_string(req.method));
                        access_entry.path = req.target;
                        access_entry.status_code = 200;
                        access_entry.request_size = req.body.size();
                        access_entry.response_size = cached->body.size();
                        access_entry.latency = latency;
                        access_entry.cache_hit = true;
                        ntonix::util::Logger::instance().access(access_entry);

                        return HttpResponse{
                            .status = http::status::ok,
                            .content_type = cached->content_type,
                            .body = cached->body,
                            .headers = {{"X-Cache", "HIT"}, {"X-Request-ID", request_id}}
                        };
                    }
                    NTONIX_LOG_DEBUG("cache", "Cache MISS: key={}", cache_key.to_string());
                }

                // Select backend using load balancer
                auto backend_selection = load_balancer->select_backend();
                if (!backend_selection) {
                    NTONIX_LOG_WARN("balancer", "No healthy backends available - returning 503");
                    return HttpResponse{
                        .status = http::status::service_unavailable,
                        .content_type = "application/json",
                        .body = R"({"error": "No healthy backends available"})",
                        .headers = {{"X-Request-ID", request_id}}
                    };
                }

                const auto& backend = backend_selection->backend;
                NTONIX_LOG_DEBUG("balancer", "Load balancer selected backend {}:{} (index={})",
                            backend.host, backend.port, backend_selection->index);

                // Forward the request to the selected backend (non-streaming)
                auto result = forwarder->forward(req, backend, req.client_ip);

                // Calculate total latency for access log
                auto end_time = std::chrono::steady_clock::now();
                auto total_latency = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

                // Log access entry
                ntonix::util::AccessLogEntry access_entry;
                access_entry.request_id = request_id;
                access_entry.client_ip = req.client_ip;
                access_entry.method = std::string(http::to_string(req.method));
                access_entry.path = req.target;
                access_entry.status_code = static_cast<int>(result.response.status);
                access_entry.request_size = req.body.size();
                access_entry.response_size = result.response.body.size();
                access_entry.latency = total_latency;
                access_entry.cache_hit = false;
                access_entry.backend_host = result.backend_host;
                access_entry.backend_port = result.backend_port;
                ntonix::util::Logger::instance().access(access_entry);

                NTONIX_LOG_DEBUG("proxy", "Backend response: {} from {}:{} in {}ms",
                            static_cast<int>(result.response.status),
                            result.backend_host, result.backend_port,
                            result.latency.count());

                if (!result.success) {
                    NTONIX_LOG_WARN("proxy", "Forward failed: {}", result.error_message);
                }

                // Cache successful responses (2xx status codes only)
                if (result.success && response_cache->is_enabled() && !bypass_cache &&
                    static_cast<int>(result.response.status) >= 200 &&
                    static_cast<int>(result.response.status) < 300) {
                    response_cache->put(cache_key, result.response.body, result.response.content_type);
                    NTONIX_LOG_DEBUG("cache", "Cached response: key={}, size={}", cache_key.to_string(), result.response.body.size());
                }

                // Add cache and request ID headers to response
                result.response.headers.push_back({"X-Cache", "MISS"});
                result.response.headers.push_back({"X-Request-ID", request_id});
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

        // Create SSL server if enabled
        std::unique_ptr<ntonix::server::SslServer> ssl_server;
        if (config.ssl.enabled) {
            try {
                // Configure SSL server
                ntonix::server::SslServerConfig ssl_server_config;
                ssl_server_config.port = config.server.ssl_port;
                ssl_server_config.bind_address = config.server.bind_address;
                ssl_server_config.ssl.cert_file = config.ssl.cert_file;
                ssl_server_config.ssl.key_file = config.ssl.key_file;
                ssl_server_config.ssl.enable_tls_1_2 = true;
                ssl_server_config.ssl.enable_tls_1_3 = true;

                // Create SSL server using the main server's io_context
                ssl_server = std::make_unique<ntonix::server::SslServer>(
                    server.get_io_context(), ssl_server_config);

                // Start accepting HTTPS connections
                ssl_server->start(
                    [request_handler, ssl_streaming_handler](asio::ip::tcp::socket socket,
                                                              boost::asio::ssl::context& ssl_ctx) {
                        ntonix::server::handle_ssl_connection(std::move(socket), ssl_ctx,
                                                               request_handler, ssl_streaming_handler);
                    }
                );

                NTONIX_LOG_INFO("ssl", "SSL server started on port {} (HTTPS)", config.server.ssl_port);
            } catch (const std::exception& e) {
                NTONIX_LOG_ERROR("ssl", "Failed to start SSL server: {}", e.what());
                NTONIX_LOG_WARN("ssl", "Continuing with HTTP-only mode");
            }
        }

        // Start health checker and connection pool after server starts (uses server's io_context)
        if (!config.backends.empty()) {
            health_checker->start();
            NTONIX_LOG_INFO("health", "Health checker started for {} backends", config.backends.size());
            connection_pool->start_cleanup();
            NTONIX_LOG_INFO("pool", "Connection pool cleanup timer started");
        }

        NTONIX_LOG_INFO("server", "Server started successfully");
        if (ssl_server && ssl_server->is_running()) {
            NTONIX_LOG_INFO("server", "HTTP on port {}, HTTPS on port {}",
                        config.server.port, config.server.ssl_port);
        } else {
            NTONIX_LOG_INFO("server", "HTTP on port {} (HTTPS disabled)", config.server.port);
        }
        NTONIX_LOG_INFO("server", "Press Ctrl+C to stop");

        // Wait for shutdown (blocks until signal received)
        server.wait();

        // Stop SSL server
        if (ssl_server) {
            ssl_server->stop();
        }

        // Stop health checker and connection pool
        health_checker->stop();
        connection_pool->stop_cleanup();

        NTONIX_LOG_INFO("server", "Server stopped gracefully");

        // Shutdown logger
        ntonix::util::Logger::instance().shutdown();
        return 0;

    } catch (const std::exception& e) {
        NTONIX_LOG_ERROR("server", "Fatal error: {}", e.what());
        ntonix::util::Logger::instance().shutdown();
        return 1;
    }
}
