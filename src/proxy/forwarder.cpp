/**
 * NTONIX - High-Performance AI Inference Gateway
 * Request Forwarder implementation
 */

#include "proxy/forwarder.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <iomanip>
#include <random>
#include <set>
#include <sstream>

namespace ntonix::proxy {

Forwarder::Forwarder(asio::io_context& io_context,
                     std::shared_ptr<ConnectionPoolManager> connection_pool,
                     const ForwarderConfig& config)
    : io_context_(io_context)
    , connection_pool_(std::move(connection_pool))
    , config_(config)
{
    spdlog::debug("Forwarder: Created with timeout={}s, connect_timeout={}s",
                  config_.request_timeout.count(), config_.connect_timeout.count());
}

ForwardResult Forwarder::forward(const server::HttpRequest& request,
                                const config::BackendConfig& backend,
                                const std::string& client_ip)
{
    ForwardResult result;
    result.backend_host = backend.host;
    result.backend_port = backend.port;

    auto start_time = std::chrono::steady_clock::now();

    spdlog::debug("Forwarder: Forwarding {} {} to {}:{}",
                  std::string(http::to_string(request.method)),
                  request.target, backend.host, backend.port);

    // Get a connection from the pool
    auto conn_guard = connection_pool_->get_connection(backend);
    if (!conn_guard) {
        result.success = false;
        result.error_message = "Failed to get connection to backend";
        result.response.status = http::status::bad_gateway;
        result.response.content_type = "application/json";
        result.response.body = R"({"error": "Failed to connect to backend"})";

        auto end_time = std::chrono::steady_clock::now();
        result.latency = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        spdlog::warn("Forwarder: Failed to get connection to {}:{}", backend.host, backend.port);
        return result;
    }

    // Build the request to send to the backend
    auto backend_request = build_backend_request(request, backend, client_ip);

    try {
        // Get the underlying socket from the pooled connection
        // conn_guard is std::optional<ConnectionGuard>, so we dereference with * to get ConnectionGuard
        // Then use -> to access the PooledConnection, and socket() to get the socket
        tcp::socket& socket = (*conn_guard)->socket();

        // Send the request directly on the socket
        // Note: For timeout handling with synchronous operations, we could use
        // async operations with a deadline timer. For now, rely on TCP defaults.
        spdlog::debug("Forwarder: Sending request to backend");
        http::write(socket, backend_request);

        // Read the response
        beast::flat_buffer buffer;
        http::response<http::string_body> backend_response;

        spdlog::debug("Forwarder: Reading response from backend");
        http::read(socket, buffer, backend_response);

        // Parse the response
        result.success = true;
        result.response = parse_backend_response(backend_response);

        auto end_time = std::chrono::steady_clock::now();
        result.latency = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        spdlog::debug("Forwarder: Received {} response from {}:{} in {}ms",
                      static_cast<int>(result.response.status),
                      backend.host, backend.port, result.latency.count());

    } catch (const beast::system_error& e) {
        result.success = false;
        conn_guard->mark_failed();  // Don't return broken connection to pool

        auto end_time = std::chrono::steady_clock::now();
        result.latency = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        // Determine appropriate status code based on error
        if (e.code() == beast::error::timeout) {
            result.error_message = "Backend request timed out";
            result.response.status = http::status::gateway_timeout;
            spdlog::warn("Forwarder: Timeout communicating with {}:{}", backend.host, backend.port);
        } else if (e.code() == asio::error::connection_refused ||
                   e.code() == asio::error::connection_reset ||
                   e.code() == asio::error::broken_pipe) {
            result.error_message = "Backend connection failed: " + e.code().message();
            result.response.status = http::status::bad_gateway;
            spdlog::warn("Forwarder: Connection error with {}:{}: {}",
                        backend.host, backend.port, e.code().message());
        } else {
            result.error_message = "Backend communication error: " + e.code().message();
            result.response.status = http::status::bad_gateway;
            spdlog::warn("Forwarder: Error communicating with {}:{}: {}",
                        backend.host, backend.port, e.code().message());
        }

        result.response.content_type = "application/json";
        result.response.body = R"({"error": ")" + result.error_message + R"("})";

    } catch (const std::exception& e) {
        result.success = false;
        conn_guard->mark_failed();

        auto end_time = std::chrono::steady_clock::now();
        result.latency = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        result.error_message = std::string("Unexpected error: ") + e.what();
        result.response.status = http::status::internal_server_error;
        result.response.content_type = "application/json";
        result.response.body = R"({"error": "Internal proxy error"})";

        spdlog::error("Forwarder: Unexpected error forwarding to {}:{}: {}",
                     backend.host, backend.port, e.what());
    }

    return result;
}

http::request<http::string_body> Forwarder::build_backend_request(
    const server::HttpRequest& request,
    const config::BackendConfig& backend,
    const std::string& client_ip)
{
    // Create a new request to the backend
    http::request<http::string_body> backend_request{request.method, request.target, 11};

    // Copy the body
    backend_request.body() = request.body;

    // Set the Host header to the backend
    backend_request.set(http::field::host, backend.host + ":" + std::to_string(backend.port));

    // Copy over relevant headers from the original request
    const auto& raw = request.raw_request;

    // Content-Type
    if (auto it = raw.find(http::field::content_type); it != raw.end()) {
        backend_request.set(http::field::content_type, it->value());
    }

    // Authorization - pass through
    if (auto it = raw.find(http::field::authorization); it != raw.end()) {
        backend_request.set(http::field::authorization, it->value());
    }

    // Accept
    if (auto it = raw.find(http::field::accept); it != raw.end()) {
        backend_request.set(http::field::accept, it->value());
    }

    // Accept-Encoding
    if (auto it = raw.find(http::field::accept_encoding); it != raw.end()) {
        backend_request.set(http::field::accept_encoding, it->value());
    }

    // User-Agent
    if (auto it = raw.find(http::field::user_agent); it != raw.end()) {
        backend_request.set(http::field::user_agent, it->value());
    }

    // Keep-Alive for connection reuse
    backend_request.set(http::field::connection, "keep-alive");

    // Add proxy headers
    if (config_.add_forwarded_headers && !client_ip.empty()) {
        // X-Forwarded-For - append to existing if present
        std::string forwarded_for = client_ip;
        if (auto it = raw.find("X-Forwarded-For"); it != raw.end()) {
            forwarded_for = std::string(it->value()) + ", " + client_ip;
        }
        backend_request.set("X-Forwarded-For", forwarded_for);

        // X-Real-IP - set to original client IP
        if (raw.find("X-Real-IP") == raw.end()) {
            backend_request.set("X-Real-IP", client_ip);
        } else {
            // Pass through existing X-Real-IP
            backend_request.set("X-Real-IP", raw.find("X-Real-IP")->value());
        }
    }

    // X-Request-ID - use existing or generate new
    std::string request_id;
    if (!request.x_request_id.empty()) {
        request_id = request.x_request_id;
    } else if (config_.generate_request_id) {
        request_id = generate_request_id();
    }
    if (!request_id.empty()) {
        backend_request.set("X-Request-ID", request_id);
    }

    // Prepare the payload (sets Content-Length)
    backend_request.prepare_payload();

    spdlog::debug("Forwarder: Built request - {} {} Host={} Content-Length={}",
                  std::string(http::to_string(backend_request.method())),
                  std::string(backend_request.target()),
                  std::string(backend_request[http::field::host]),
                  backend_request.body().size());

    return backend_request;
}

std::string Forwarder::generate_request_id() {
    // Generate a UUID-like request ID
    static thread_local std::random_device rd;
    static thread_local std::mt19937_64 gen(rd());
    static thread_local std::uniform_int_distribution<std::uint64_t> dis;

    std::uint64_t part1 = dis(gen);
    std::uint64_t part2 = dis(gen);

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    ss << std::setw(8) << ((part1 >> 32) & 0xFFFFFFFF) << "-";
    ss << std::setw(4) << ((part1 >> 16) & 0xFFFF) << "-";
    ss << std::setw(4) << (part1 & 0xFFFF) << "-";
    ss << std::setw(4) << ((part2 >> 48) & 0xFFFF) << "-";
    ss << std::setw(12) << (part2 & 0xFFFFFFFFFFFF);

    return ss.str();
}

server::HttpResponse Forwarder::parse_backend_response(
    const http::response<http::string_body>& response)
{
    server::HttpResponse result;

    result.status = response.result();
    result.body = response.body();

    // Get Content-Type
    if (auto it = response.find(http::field::content_type); it != response.end()) {
        result.content_type = std::string(it->value());
    }

    // Copy relevant headers from backend response
    // Skip hop-by-hop headers that shouldn't be forwarded
    static const std::set<http::field> hop_by_hop = {
        http::field::connection,
        http::field::keep_alive,
        http::field::proxy_authenticate,
        http::field::proxy_authorization,
        http::field::te,
        http::field::trailer,
        http::field::transfer_encoding,
        http::field::upgrade,
    };

    for (const auto& header : response) {
        // Skip hop-by-hop headers
        if (hop_by_hop.count(header.name()) > 0) {
            continue;
        }
        // Skip Content-Type (handled separately)
        if (header.name() == http::field::content_type) {
            continue;
        }
        // Skip Server (we'll set our own)
        if (header.name() == http::field::server) {
            continue;
        }

        result.headers.emplace_back(
            std::string(header.name_string()),
            std::string(header.value())
        );
    }

    return result;
}

bool Forwarder::is_streaming_request(const server::HttpRequest& request) {
    // Check if the request body contains "stream": true (OpenAI API format)
    // This is a simple check; a more robust implementation would parse the JSON
    if (request.body.find("\"stream\"") != std::string::npos) {
        // Check for "stream": true or "stream":true
        if (request.body.find("\"stream\": true") != std::string::npos ||
            request.body.find("\"stream\":true") != std::string::npos) {
            return true;
        }
    }

    // Also check Accept header for text/event-stream
    const auto& raw = request.raw_request;
    if (auto it = raw.find(http::field::accept); it != raw.end()) {
        std::string accept{it->value()};
        if (accept.find("text/event-stream") != std::string::npos) {
            return true;
        }
    }

    return false;
}

ForwardResult Forwarder::forward_with_streaming(const server::HttpRequest& request,
                                                const config::BackendConfig& backend,
                                                beast::tcp_stream& client_stream,
                                                const std::string& client_ip)
{
    ForwardResult result;
    result.backend_host = backend.host;
    result.backend_port = backend.port;

    auto start_time = std::chrono::steady_clock::now();

    // Check if this should be a streaming request
    bool expect_streaming = is_streaming_request(request);

    spdlog::debug("Forwarder: Forwarding {} {} to {}:{} (streaming={})",
                  std::string(http::to_string(request.method)),
                  request.target, backend.host, backend.port, expect_streaming);

    // Get a connection from the pool
    auto conn_guard = connection_pool_->get_connection(backend);
    if (!conn_guard) {
        result.success = false;
        result.error_message = "Failed to get connection to backend";
        result.response.status = http::status::bad_gateway;
        result.response.content_type = "application/json";
        result.response.body = R"({"error": "Failed to connect to backend"})";

        auto end_time = std::chrono::steady_clock::now();
        result.latency = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        spdlog::warn("Forwarder: Failed to get connection to {}:{}", backend.host, backend.port);
        return result;
    }

    // Build the request to send to the backend
    auto backend_request = build_backend_request(request, backend, client_ip);

    try {
        tcp::socket& socket = (*conn_guard)->socket();

        // Send the request to backend
        spdlog::debug("Forwarder: Sending request to backend");
        http::write(socket, backend_request);

        // Read just the response header first to determine if streaming
        beast::flat_buffer buffer;
        http::response_parser<http::string_body> parser;
        parser.body_limit(boost::none);  // No body limit for streaming

        // Read the header
        http::read_header(socket, buffer, parser);

        auto& response_header = parser.get();

        spdlog::debug("Forwarder: Got response status={}, Content-Type={}",
                      static_cast<int>(response_header.result()),
                      response_header.count(http::field::content_type) > 0 ?
                          std::string(response_header[http::field::content_type]) : "(none)");

        // Check if this is a streaming response
        if (expect_streaming && StreamPipe::is_streaming_response(response_header.base())) {
            spdlog::info("Forwarder: Streaming response detected - using zero-copy forwarding");

            // Get any body data already read with the header
            std::string initial_body;
            if (parser.is_done()) {
                // Entire response was in header read
                initial_body = parser.get().body();
            } else {
                // There may be some body data in the buffer
                auto remaining = buffer.data();
                initial_body = std::string(
                    static_cast<const char*>(remaining.data()),
                    remaining.size());
                buffer.consume(remaining.size());
            }

            // Create stream pipe and forward
            auto stream_pipe = make_stream_pipe(io_context_, config_.stream_config);
            result.stream_result = stream_pipe->forward_stream(
                socket, client_stream, response_header.base(), initial_body);

            result.is_streaming = true;
            result.success = result.stream_result.success;

            if (!result.success) {
                result.error_message = result.stream_result.error_message;
            }

            // After streaming, connection should not be reused
            conn_guard->mark_failed();

            auto end_time = std::chrono::steady_clock::now();
            result.latency = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

            spdlog::info("Forwarder: Streaming complete - {} bytes forwarded in {}ms",
                        result.stream_result.bytes_forwarded, result.latency.count());

        } else {
            // Non-streaming response - read the full body
            spdlog::debug("Forwarder: Non-streaming response - reading full body");

            // Continue reading the rest of the response
            http::read(socket, buffer, parser);

            // Parse the response
            result.success = true;
            result.response = parse_backend_response(parser.get());
            result.is_streaming = false;

            auto end_time = std::chrono::steady_clock::now();
            result.latency = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

            spdlog::debug("Forwarder: Received {} response from {}:{} in {}ms",
                          static_cast<int>(result.response.status),
                          backend.host, backend.port, result.latency.count());
        }

    } catch (const beast::system_error& e) {
        result.success = false;
        conn_guard->mark_failed();

        auto end_time = std::chrono::steady_clock::now();
        result.latency = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        if (e.code() == beast::error::timeout) {
            result.error_message = "Backend request timed out";
            result.response.status = http::status::gateway_timeout;
            spdlog::warn("Forwarder: Timeout communicating with {}:{}", backend.host, backend.port);
        } else if (e.code() == asio::error::connection_refused ||
                   e.code() == asio::error::connection_reset ||
                   e.code() == asio::error::broken_pipe) {
            result.error_message = "Backend connection failed: " + e.code().message();
            result.response.status = http::status::bad_gateway;
            spdlog::warn("Forwarder: Connection error with {}:{}: {}",
                        backend.host, backend.port, e.code().message());
        } else {
            result.error_message = "Backend communication error: " + e.code().message();
            result.response.status = http::status::bad_gateway;
            spdlog::warn("Forwarder: Error communicating with {}:{}: {}",
                        backend.host, backend.port, e.code().message());
        }

        result.response.content_type = "application/json";
        result.response.body = R"({"error": ")" + result.error_message + R"("})";

    } catch (const std::exception& e) {
        result.success = false;
        conn_guard->mark_failed();

        auto end_time = std::chrono::steady_clock::now();
        result.latency = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        result.error_message = std::string("Unexpected error: ") + e.what();
        result.response.status = http::status::internal_server_error;
        result.response.content_type = "application/json";
        result.response.body = R"({"error": "Internal proxy error"})";

        spdlog::error("Forwarder: Unexpected error forwarding to {}:{}: {}",
                     backend.host, backend.port, e.what());
    }

    return result;
}

} // namespace ntonix::proxy
