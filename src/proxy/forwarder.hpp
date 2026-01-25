/**
 * NTONIX - High-Performance AI Inference Gateway
 * Request Forwarder - Forwards HTTP requests to backend servers
 */

#ifndef NTONIX_PROXY_FORWARDER_HPP
#define NTONIX_PROXY_FORWARDER_HPP

#include "config/config.hpp"
#include "server/connection.hpp"
#include "proxy/connection_pool.hpp"
#include "proxy/stream_pipe.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace ntonix::proxy {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

/**
 * Configuration for request forwarding
 */
struct ForwarderConfig {
    std::chrono::seconds request_timeout{30};     // Timeout for backend response
    std::chrono::seconds connect_timeout{5};      // Timeout for establishing connection
    bool add_forwarded_headers{true};             // Add X-Forwarded-For, X-Real-IP
    bool generate_request_id{true};               // Generate X-Request-ID if not present
    std::size_t max_retries{0};                   // Retry count on connection failure (0 = no retry)
    StreamPipeConfig stream_config{};              // Configuration for streaming responses
};

/**
 * Result of forwarding a request
 */
struct ForwardResult {
    bool success{false};
    server::HttpResponse response;
    std::string error_message;
    std::chrono::milliseconds latency{0};

    // Backend that handled the request
    std::string backend_host;
    std::uint16_t backend_port{0};

    // Streaming-specific fields
    bool is_streaming{false};               // True if response was streamed
    StreamResult stream_result{};           // Details of streaming (if is_streaming)
};

/**
 * Request Forwarder - forwards HTTP requests to backend servers
 *
 * Features:
 * - Uses connection pooling for efficient backend connections
 * - Adds proxy headers (X-Forwarded-For, X-Real-IP, X-Request-ID)
 * - Configurable timeouts for connect and request
 * - Graceful error handling with detailed error messages
 */
class Forwarder : public std::enable_shared_from_this<Forwarder> {
public:
    using Ptr = std::shared_ptr<Forwarder>;

    /**
     * Create a new forwarder
     * @param io_context Asio io_context for async operations
     * @param connection_pool Connection pool manager for backend connections
     * @param config Forwarder configuration
     */
    Forwarder(asio::io_context& io_context,
              std::shared_ptr<ConnectionPoolManager> connection_pool,
              const ForwarderConfig& config = {});

    ~Forwarder() = default;

    // Non-copyable
    Forwarder(const Forwarder&) = delete;
    Forwarder& operator=(const Forwarder&) = delete;

    /**
     * Forward a request to a backend synchronously
     * @param request The HTTP request to forward
     * @param backend The backend to forward to
     * @param client_ip The client's IP address (for X-Forwarded-For)
     * @return ForwardResult with response or error
     */
    ForwardResult forward(const server::HttpRequest& request,
                         const config::BackendConfig& backend,
                         const std::string& client_ip = "");

    /**
     * Forward a request with streaming response support
     * If the backend returns a streaming response (SSE), this streams directly to the client.
     * Non-streaming responses are handled normally.
     *
     * @param request The HTTP request to forward
     * @param backend The backend to forward to
     * @param client_stream The client's TCP stream for direct streaming
     * @param client_ip The client's IP address (for X-Forwarded-For)
     * @return ForwardResult with response or streaming details
     */
    ForwardResult forward_with_streaming(const server::HttpRequest& request,
                                         const config::BackendConfig& backend,
                                         beast::tcp_stream& client_stream,
                                         const std::string& client_ip = "");

    /**
     * Check if a request should be handled with streaming
     * (Based on request headers, e.g., Accept: text/event-stream)
     */
    static bool is_streaming_request(const server::HttpRequest& request);

    /**
     * Get the configuration
     */
    const ForwarderConfig& config() const { return config_; }

private:
    /**
     * Build the backend request with proxy headers
     */
    http::request<http::string_body> build_backend_request(
        const server::HttpRequest& request,
        const config::BackendConfig& backend,
        const std::string& client_ip);

    /**
     * Generate a unique request ID
     */
    static std::string generate_request_id();

    /**
     * Parse backend response into HttpResponse
     */
    server::HttpResponse parse_backend_response(
        const http::response<http::string_body>& response);

    asio::io_context& io_context_;
    std::shared_ptr<ConnectionPoolManager> connection_pool_;
    ForwarderConfig config_;
};

/**
 * Create a forwarder instance
 */
inline Forwarder::Ptr make_forwarder(
    asio::io_context& io_context,
    std::shared_ptr<ConnectionPoolManager> connection_pool,
    const ForwarderConfig& config = {})
{
    return std::make_shared<Forwarder>(io_context, connection_pool, config);
}

} // namespace ntonix::proxy

#endif // NTONIX_PROXY_FORWARDER_HPP
