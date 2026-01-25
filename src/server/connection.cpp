/**
 * NTONIX - High-Performance AI Inference Gateway
 * Connection implementation - HTTP/1.1 request parsing with Boost.Beast
 */

#include "server/connection.hpp"

#include <spdlog/spdlog.h>

#include <chrono>

namespace ntonix::server {

Connection::Connection(tcp::socket socket, RequestHandler handler)
    : stream_(std::move(socket))
    , handler_(std::move(handler))
{
    // Capture client endpoint before moving socket
    beast::error_code ec;
    auto endpoint = stream_.socket().remote_endpoint(ec);
    if (!ec) {
        client_ip_ = endpoint.address().to_string();
        client_port_ = endpoint.port();
    }

    // Set reasonable timeout
    stream_.expires_after(std::chrono::seconds(30));
}

void Connection::start() {
    // We need to be executing within a strand to perform async operations
    // on the I/O objects in this session.
    asio::dispatch(
        stream_.get_executor(),
        beast::bind_front_handler(&Connection::do_read, shared_from_this())
    );
}

void Connection::close() {
    beast::error_code ec;
    stream_.socket().shutdown(tcp::socket::shutdown_both, ec);
    // Ignore errors on shutdown - socket may already be closed
}

void Connection::do_read() {
    // Clear the request for the next read
    request_ = {};

    // Set timeout for this read operation
    stream_.expires_after(std::chrono::seconds(30));

    // Read a request
    http::async_read(
        stream_,
        buffer_,
        request_,
        beast::bind_front_handler(&Connection::on_read, shared_from_this())
    );
}

void Connection::on_read(beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);

    // Client closed connection
    if (ec == http::error::end_of_stream) {
        spdlog::debug("Connection: Client closed connection");
        close();
        return;
    }

    // Handle errors
    if (ec) {
        if (ec == beast::error::timeout) {
            spdlog::debug("Connection: Read timeout");
        } else if (ec != asio::error::operation_aborted) {
            // Log parsing errors specifically for 400 Bad Request
            if (ec == http::error::bad_method ||
                ec == http::error::bad_target ||
                ec == http::error::bad_version ||
                ec == http::error::bad_content_length ||
                ec == http::error::partial_message) {
                spdlog::warn("Connection: Malformed request - {}", ec.message());
                // Send 400 Bad Request response
                response_ = build_error_response(
                    http::status::bad_request,
                    "Malformed HTTP request: " + ec.message()
                );
                do_write();
                return;
            }
            spdlog::debug("Connection: Read error - {}", ec.message());
        }
        close();
        return;
    }

    // Log the request
    spdlog::debug("Connection: {} {} HTTP/{}.{}",
                  std::string(http::to_string(request_.method())),
                  std::string(request_.target()),
                  request_.version() / 10,
                  request_.version() % 10);

    // Validate HTTP version (require HTTP/1.0 or HTTP/1.1)
    if (request_.version() != 10 && request_.version() != 11) {
        spdlog::warn("Connection: Unsupported HTTP version {}.{}",
                     request_.version() / 10, request_.version() % 10);
        response_ = build_error_response(
            http::status::http_version_not_supported,
            "Only HTTP/1.0 and HTTP/1.1 are supported"
        );
        do_write();
        return;
    }

    // Store keep-alive preference
    keep_alive_ = request_.keep_alive();

    // Parse the request and invoke handler
    try {
        HttpRequest parsed_req = parse_request(request_);
        HttpResponse resp = handler_(parsed_req);
        response_ = build_response(resp);
    } catch (const std::exception& e) {
        spdlog::error("Connection: Handler exception - {}", e.what());
        response_ = build_error_response(
            http::status::internal_server_error,
            "Internal server error"
        );
    }

    // Send the response
    do_write();
}

void Connection::do_write() {
    // Set keep-alive based on request
    response_.set(http::field::connection, keep_alive_ ? "keep-alive" : "close");

    // Ensure content-length is set
    response_.prepare_payload();

    // Set timeout
    stream_.expires_after(std::chrono::seconds(30));

    http::async_write(
        stream_,
        response_,
        beast::bind_front_handler(
            &Connection::on_write,
            shared_from_this()
        )
    );
}

void Connection::on_write(beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);

    if (ec) {
        if (ec != asio::error::operation_aborted) {
            spdlog::debug("Connection: Write error - {}", ec.message());
        }
        close();
        return;
    }

    spdlog::debug("Connection: Sent {} response",
                  static_cast<int>(response_.result()));

    if (!keep_alive_) {
        // Close the connection gracefully
        close();
        return;
    }

    // Clear for next request
    response_ = {};

    // Read another request (keep-alive)
    do_read();
}

HttpRequest Connection::parse_request(const http::request<http::string_body>& req) {
    HttpRequest parsed;

    parsed.method = req.method();
    parsed.target = std::string(req.target());
    parsed.version = req.version();
    parsed.body = req.body();
    parsed.raw_request = req;

    // Client connection info
    parsed.client_ip = client_ip_;
    parsed.client_port = client_port_;

    // Extract common headers (case-insensitive with Beast)
    if (auto it = req.find(http::field::host); it != req.end()) {
        parsed.host = std::string(it->value());
    }
    if (auto it = req.find(http::field::content_type); it != req.end()) {
        parsed.content_type = std::string(it->value());
    }
    if (auto it = req.find(http::field::authorization); it != req.end()) {
        parsed.authorization = std::string(it->value());
    }
    // X-Request-ID is a custom header, search by name
    if (auto it = req.find("X-Request-ID"); it != req.end()) {
        parsed.x_request_id = std::string(it->value());
    }

    return parsed;
}

http::response<http::string_body> Connection::build_response(const HttpResponse& resp) {
    http::response<http::string_body> response{resp.status, request_.version()};

    response.set(http::field::server, "NTONIX/0.1.0");
    response.set(http::field::content_type, resp.content_type);

    // Add custom headers
    for (const auto& [name, value] : resp.headers) {
        response.set(name, value);
    }

    response.body() = resp.body;

    return response;
}

http::response<http::string_body> Connection::build_error_response(
    http::status status, const std::string& message)
{
    http::response<http::string_body> response{status, request_.version()};

    response.set(http::field::server, "NTONIX/0.1.0");
    response.set(http::field::content_type, "application/json");

    // JSON error response
    response.body() = "{\"error\": \"" + message + "\"}";

    return response;
}

void handle_connection(tcp::socket socket, RequestHandler handler) {
    std::make_shared<Connection>(std::move(socket), std::move(handler))->start();
}

} // namespace ntonix::server
