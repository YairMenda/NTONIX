/**
 * NTONIX - High-Performance AI Inference Gateway
 * SSL Connection implementation - HTTPS connection handling with TLS termination
 */

#include "server/ssl_connection.hpp"

#include <spdlog/spdlog.h>

#include <chrono>

namespace ntonix::server {

SslConnection::SslConnection(tcp::socket socket, ssl::context& ssl_ctx,
                             RequestHandler handler,
                             SslStreamingRequestHandler streaming_handler)
    : stream_(beast::tcp_stream(std::move(socket)), ssl_ctx)
    , handler_(std::move(handler))
    , streaming_handler_(std::move(streaming_handler))
{
    // Capture client endpoint before starting handshake
    beast::error_code ec;
    auto endpoint = beast::get_lowest_layer(stream_).socket().remote_endpoint(ec);
    if (!ec) {
        client_ip_ = endpoint.address().to_string();
        client_port_ = endpoint.port();
    }
}

void SslConnection::start() {
    // Start with SSL handshake
    asio::dispatch(
        beast::get_lowest_layer(stream_).get_executor(),
        beast::bind_front_handler(&SslConnection::do_handshake, shared_from_this())
    );
}

void SslConnection::close() {
    // Initiate graceful SSL shutdown
    do_shutdown();
}

void SslConnection::do_handshake() {
    // Set timeout for handshake
    beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(10));

    // Perform SSL handshake
    stream_.async_handshake(
        ssl::stream_base::server,
        beast::bind_front_handler(&SslConnection::on_handshake, shared_from_this())
    );
}

void SslConnection::on_handshake(beast::error_code ec) {
    if (ec) {
        if (ec == asio::error::operation_aborted) {
            spdlog::debug("SSL Connection: Handshake aborted");
            return;
        }

        // Log handshake failures for debugging
        if (ec.category() == asio::error::get_ssl_category()) {
            spdlog::warn("SSL Connection: Handshake failed from {}:{} - SSL error: {}",
                        client_ip_.empty() ? "(unknown)" : client_ip_,
                        client_port_,
                        ec.message());
        } else if (ec == beast::error::timeout) {
            spdlog::debug("SSL Connection: Handshake timeout from {}:{}",
                         client_ip_.empty() ? "(unknown)" : client_ip_,
                         client_port_);
        } else {
            spdlog::debug("SSL Connection: Handshake error from {}:{} - {}",
                         client_ip_.empty() ? "(unknown)" : client_ip_,
                         client_port_,
                         ec.message());
        }
        return;
    }

    spdlog::info("SSL Connection: Handshake complete from {}:{}",
                client_ip_.empty() ? "(unknown)" : client_ip_,
                client_port_);

    // Start reading HTTP requests
    do_read();
}

void SslConnection::do_read() {
    // Clear the request for the next read
    request_ = {};

    // Set timeout for this read operation
    beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));

    // Read a request over SSL
    http::async_read(
        stream_,
        buffer_,
        request_,
        beast::bind_front_handler(&SslConnection::on_read, shared_from_this())
    );
}

void SslConnection::on_read(beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);

    // Client closed connection
    if (ec == http::error::end_of_stream) {
        spdlog::debug("SSL Connection: Client closed connection");
        do_shutdown();
        return;
    }

    // Handle SSL-specific errors
    if (ec == asio::ssl::error::stream_truncated) {
        spdlog::debug("SSL Connection: Stream truncated (client disconnect)");
        return;
    }

    // Handle errors
    if (ec) {
        if (ec == beast::error::timeout) {
            spdlog::debug("SSL Connection: Read timeout");
        } else if (ec != asio::error::operation_aborted) {
            // Log parsing errors specifically for 400 Bad Request
            if (ec == http::error::bad_method ||
                ec == http::error::bad_target ||
                ec == http::error::bad_version ||
                ec == http::error::bad_content_length ||
                ec == http::error::partial_message) {
                spdlog::warn("SSL Connection: Malformed request - {}", ec.message());
                // Send 400 Bad Request response
                response_ = build_error_response(
                    http::status::bad_request,
                    "Malformed HTTP request: " + ec.message()
                );
                do_write();
                return;
            }
            spdlog::debug("SSL Connection: Read error - {}", ec.message());
        }
        do_shutdown();
        return;
    }

    // Log the request
    spdlog::debug("SSL Connection: {} {} HTTP/{}.{}",
                  std::string(http::to_string(request_.method())),
                  std::string(request_.target()),
                  request_.version() / 10,
                  request_.version() % 10);

    // Validate HTTP version (require HTTP/1.0 or HTTP/1.1)
    if (request_.version() != 10 && request_.version() != 11) {
        spdlog::warn("SSL Connection: Unsupported HTTP version {}.{}",
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

        // Try streaming handler first if available
        streaming_handled_ = false;
        if (streaming_handler_) {
            streaming_handled_ = streaming_handler_(parsed_req, stream_);
        }

        // If streaming handler didn't handle it, use normal handler
        if (!streaming_handled_) {
            HttpResponse resp = handler_(parsed_req);
            response_ = build_response(resp);
        }
    } catch (const std::exception& e) {
        spdlog::error("SSL Connection: Handler exception - {}", e.what());
        if (!streaming_handled_) {
            response_ = build_error_response(
                http::status::internal_server_error,
                "Internal server error"
            );
        }
    }

    // If streaming was handled, response was already sent
    if (streaming_handled_) {
        // For streaming, we don't keep the connection alive after
        do_shutdown();
        return;
    }

    // Send the response
    do_write();
}

void SslConnection::do_write() {
    // Set keep-alive based on request
    response_.set(http::field::connection, keep_alive_ ? "keep-alive" : "close");

    // Ensure content-length is set
    response_.prepare_payload();

    // Set timeout
    beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));

    http::async_write(
        stream_,
        response_,
        beast::bind_front_handler(&SslConnection::on_write, shared_from_this())
    );
}

void SslConnection::on_write(beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);

    if (ec) {
        if (ec != asio::error::operation_aborted) {
            spdlog::debug("SSL Connection: Write error - {}", ec.message());
        }
        do_shutdown();
        return;
    }

    spdlog::debug("SSL Connection: Sent {} response",
                  static_cast<int>(response_.result()));

    if (!keep_alive_) {
        // Close the connection gracefully
        do_shutdown();
        return;
    }

    // Clear for next request
    response_ = {};

    // Read another request (keep-alive)
    do_read();
}

void SslConnection::do_shutdown() {
    // Set timeout for shutdown
    beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(5));

    // Perform SSL shutdown
    stream_.async_shutdown(
        beast::bind_front_handler(&SslConnection::on_shutdown, shared_from_this())
    );
}

void SslConnection::on_shutdown(beast::error_code ec) {
    // These errors are expected and not problematic:
    // - stream_truncated: Client closed connection without proper SSL shutdown
    // - operation_aborted: We're shutting down
    // - eof: Connection ended normally
    if (ec && ec != asio::ssl::error::stream_truncated &&
        ec != asio::error::operation_aborted &&
        ec != asio::error::eof) {
        spdlog::debug("SSL Connection: Shutdown error - {}", ec.message());
    }
    // Connection is now closed
}

HttpRequest SslConnection::parse_request(const http::request<http::string_body>& req) {
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

http::response<http::string_body> SslConnection::build_response(const HttpResponse& resp) {
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

http::response<http::string_body> SslConnection::build_error_response(
    http::status status, const std::string& message)
{
    http::response<http::string_body> response{status, request_.version()};

    response.set(http::field::server, "NTONIX/0.1.0");
    response.set(http::field::content_type, "application/json");

    // JSON error response
    response.body() = "{\"error\": \"" + message + "\"}";

    return response;
}

void handle_ssl_connection(tcp::socket socket, ssl::context& ssl_ctx,
                           RequestHandler handler,
                           SslStreamingRequestHandler streaming_handler) {
    std::make_shared<SslConnection>(std::move(socket), ssl_ctx,
                                     std::move(handler),
                                     std::move(streaming_handler))->start();
}

} // namespace ntonix::server
