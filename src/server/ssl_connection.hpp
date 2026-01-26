/**
 * NTONIX - High-Performance AI Inference Gateway
 * SSL Connection handler - HTTPS connection handling with TLS termination
 */

#ifndef NTONIX_SERVER_SSL_CONNECTION_HPP
#define NTONIX_SERVER_SSL_CONNECTION_HPP

#include "server/connection.hpp"
#include "server/ssl_context.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <functional>
#include <memory>
#include <string>

namespace ntonix::server {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

/**
 * SSL stream type used for connections
 */
using ssl_stream = ssl::stream<beast::tcp_stream>;

/**
 * Streaming request handler for SSL connections
 * Takes the request and the SSL stream for direct streaming.
 * Returns true if streaming was handled (no normal response needed),
 * false if a normal HttpResponse should be sent.
 */
using SslStreamingRequestHandler = std::function<bool(const HttpRequest&, ssl_stream&)>;

/**
 * SSL Connection class - manages a single HTTPS client connection
 *
 * Uses Boost.Beast with SSL for HTTP parsing and supports:
 * - TLS 1.2/1.3 handshake
 * - HTTP/1.1 request parsing over encrypted channel
 * - Chunked transfer encoding
 * - Keep-alive connections
 * - POST requests with JSON body
 * - Graceful SSL shutdown
 */
class SslConnection : public std::enable_shared_from_this<SslConnection> {
public:
    /**
     * Create a new SSL connection
     * @param socket The accepted TCP socket
     * @param ssl_ctx SSL context for encryption
     * @param handler The request handler callback
     * @param streaming_handler Optional streaming request handler for SSE/streaming responses
     */
    explicit SslConnection(tcp::socket socket, ssl::context& ssl_ctx,
                           RequestHandler handler,
                           SslStreamingRequestHandler streaming_handler = nullptr);

    ~SslConnection() = default;

    // Non-copyable, non-movable
    SslConnection(const SslConnection&) = delete;
    SslConnection& operator=(const SslConnection&) = delete;
    SslConnection(SslConnection&&) = delete;
    SslConnection& operator=(SslConnection&&) = delete;

    /**
     * Start processing the connection asynchronously
     * Initiates SSL handshake followed by HTTP request handling
     */
    void start();

    /**
     * Close the connection gracefully with SSL shutdown
     */
    void close();

private:
    void do_handshake();
    void on_handshake(beast::error_code ec);
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void do_write();
    void on_write(beast::error_code ec, std::size_t bytes_transferred);
    void do_shutdown();
    void on_shutdown(beast::error_code ec);

    HttpRequest parse_request(const http::request<http::string_body>& req);
    http::response<http::string_body> build_response(const HttpResponse& resp);
    http::response<http::string_body> build_error_response(http::status status, const std::string& message);

    ssl_stream stream_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> request_;
    http::response<http::string_body> response_;
    RequestHandler handler_;
    SslStreamingRequestHandler streaming_handler_;
    bool keep_alive_{false};
    bool streaming_handled_{false};

    // Client connection info (captured at connection time)
    std::string client_ip_;
    std::uint16_t client_port_{0};
};

/**
 * Create and start an SSL connection
 * Helper function for use with SSL server
 */
void handle_ssl_connection(tcp::socket socket, ssl::context& ssl_ctx,
                           RequestHandler handler,
                           SslStreamingRequestHandler streaming_handler = nullptr);

} // namespace ntonix::server

#endif // NTONIX_SERVER_SSL_CONNECTION_HPP
