/**
 * NTONIX - High-Performance AI Inference Gateway
 * Connection handler - HTTP/1.1 request parsing with Boost.Beast
 */

#ifndef NTONIX_SERVER_CONNECTION_HPP
#define NTONIX_SERVER_CONNECTION_HPP

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <functional>
#include <memory>
#include <string>

namespace ntonix::server {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

/**
 * Parsed HTTP request information
 */
struct HttpRequest {
    http::verb method{http::verb::unknown};
    std::string target;
    unsigned version{11};  // HTTP/1.1 = 11

    // Common headers
    std::string host;
    std::string content_type;
    std::string authorization;
    std::string x_request_id;

    // Request body (for POST/PUT)
    std::string body;

    // Client connection info
    std::string client_ip;
    std::uint16_t client_port{0};

    // Full headers access
    http::request<http::string_body> raw_request;
};

/**
 * HTTP response structure
 */
struct HttpResponse {
    http::status status{http::status::ok};
    std::string content_type{"text/plain"};
    std::string body;

    // Additional headers (optional)
    std::vector<std::pair<std::string, std::string>> headers{};
};

/**
 * Request handler callback type
 * Return true to keep connection alive, false to close
 */
using RequestHandler = std::function<HttpResponse(const HttpRequest&)>;

/**
 * Streaming request handler callback type
 * Takes the request and the client's TCP stream for direct streaming.
 * Returns true if streaming was handled (no normal response needed),
 * false if a normal HttpResponse should be sent.
 */
using StreamingRequestHandler = std::function<bool(const HttpRequest&, beast::tcp_stream&)>;

/**
 * Connection class - manages a single HTTP/1.1 client connection
 *
 * Uses Boost.Beast for HTTP parsing and supports:
 * - HTTP/1.1 request parsing
 * - Chunked transfer encoding
 * - Keep-alive connections
 * - POST requests with JSON body
 */
class Connection : public std::enable_shared_from_this<Connection> {
public:
    /**
     * Create a new connection
     * @param socket The accepted TCP socket
     * @param handler The request handler callback
     * @param streaming_handler Optional streaming request handler for SSE/streaming responses
     */
    explicit Connection(tcp::socket socket, RequestHandler handler,
                        StreamingRequestHandler streaming_handler = nullptr);

    ~Connection() = default;

    // Non-copyable, non-movable
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) = delete;
    Connection& operator=(Connection&&) = delete;

    /**
     * Start processing the connection asynchronously
     */
    void start();

    /**
     * Close the connection gracefully
     */
    void close();

private:
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void do_write();
    void on_write(beast::error_code ec, std::size_t bytes_transferred);

    HttpRequest parse_request(const http::request<http::string_body>& req);
    http::response<http::string_body> build_response(const HttpResponse& resp);
    http::response<http::string_body> build_error_response(http::status status, const std::string& message);

    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> request_;
    http::response<http::string_body> response_;
    RequestHandler handler_;
    StreamingRequestHandler streaming_handler_;
    bool keep_alive_{false};
    bool streaming_handled_{false};  // True if streaming handler took over

    // Client connection info (captured at connection time)
    std::string client_ip_;
    std::uint16_t client_port_{0};
};

/**
 * Create and start a connection
 * Helper function for use with Server::start()
 */
void handle_connection(tcp::socket socket, RequestHandler handler,
                       StreamingRequestHandler streaming_handler = nullptr);

} // namespace ntonix::server

#endif // NTONIX_SERVER_CONNECTION_HPP
