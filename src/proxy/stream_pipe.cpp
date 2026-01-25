/**
 * NTONIX - High-Performance AI Inference Gateway
 * Stream Pipe implementation - Zero-copy SSE stream forwarding
 */

#include "proxy/stream_pipe.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

namespace ntonix::proxy {

StreamPipe::StreamPipe(asio::io_context& io_context, const StreamPipeConfig& config)
    : io_context_(io_context)
    , config_(config)
    , read_buffer_(config.buffer_size)
{
    spdlog::debug("StreamPipe: Created with buffer_size={}, read_timeout={}s",
                  config_.buffer_size, config_.read_timeout.count());
}

StreamPipe::~StreamPipe() {
    spdlog::debug("StreamPipe: Destroyed");
}

bool StreamPipe::is_streaming_response(const http::response_header<>& header) {
    // Check status - only stream successful responses
    if (header.result() != http::status::ok) {
        return false;
    }

    // Check Content-Type for SSE
    auto content_type_it = header.find(http::field::content_type);
    if (content_type_it != header.end()) {
        std::string content_type{content_type_it->value()};
        // OpenAI uses text/event-stream for streaming responses
        if (content_type.find("text/event-stream") != std::string::npos) {
            return true;
        }
    }

    // Check for Transfer-Encoding: chunked (may indicate streaming)
    auto transfer_encoding_it = header.find(http::field::transfer_encoding);
    if (transfer_encoding_it != header.end()) {
        std::string transfer_encoding{transfer_encoding_it->value()};
        if (transfer_encoding.find("chunked") != std::string::npos) {
            // Also check if Content-Type indicates it's not a simple JSON response
            if (content_type_it != header.end()) {
                std::string content_type{content_type_it->value()};
                // Don't stream regular JSON responses
                if (content_type.find("application/json") != std::string::npos) {
                    return false;
                }
            }
            return true;
        }
    }

    return false;
}

bool StreamPipe::contains_done_marker(const char* data, std::size_t size) const {
    if (!config_.detect_done_marker || size < 6) {
        return false;
    }

    // Look for "data: [DONE]" which is the SSE marker for end of stream
    // Also check for just "[DONE]" in case it's not prefixed
    static const char* done_markers[] = {
        "data: [DONE]",
        "[DONE]"
    };

    for (const char* marker : done_markers) {
        std::size_t marker_len = std::strlen(marker);
        if (size >= marker_len) {
            // Search for marker in data
            auto it = std::search(data, data + size, marker, marker + marker_len);
            if (it != data + size) {
                return true;
            }
        }
    }

    return false;
}

std::size_t StreamPipe::write_chunk_to_client(
    beast::tcp_stream& client_stream,
    const char* data,
    std::size_t size,
    beast::error_code& ec)
{
    if (size == 0) {
        return 0;
    }

    if (config_.forward_chunked) {
        // Build chunked transfer encoding format:
        // size-in-hex\r\n
        // data\r\n

        // Convert size to hex
        char size_hex[32];
        int hex_len = std::snprintf(size_hex, sizeof(size_hex), "%zx\r\n", size);

        // Use const_buffer for zero-copy - create array of buffers to send
        std::array<asio::const_buffer, 3> buffers = {{
            asio::buffer(size_hex, hex_len),
            asio::buffer(data, size),
            asio::buffer("\r\n", 2)
        }};

        // Write all buffers in one call (zero-copy from data pointer)
        std::size_t bytes_written = asio::write(client_stream, buffers, ec);
        if (ec) {
            spdlog::debug("StreamPipe: Error writing chunk to client: {}", ec.message());
            return 0;
        }

        return size;  // Return the actual data bytes, not chunk overhead
    } else {
        // Direct write without chunked encoding (zero-copy)
        asio::const_buffer buf(data, size);
        std::size_t bytes_written = asio::write(client_stream, buf, ec);
        if (ec) {
            spdlog::debug("StreamPipe: Error writing to client: {}", ec.message());
            return 0;
        }
        return bytes_written;
    }
}

bool StreamPipe::write_final_chunk(beast::tcp_stream& client_stream, beast::error_code& ec) {
    if (config_.forward_chunked) {
        // Final chunk for chunked transfer encoding: 0\r\n\r\n
        static const char final_chunk[] = "0\r\n\r\n";
        asio::write(client_stream, asio::buffer(final_chunk, 5), ec);
        if (ec) {
            spdlog::debug("StreamPipe: Error writing final chunk: {}", ec.message());
            return false;
        }
    }
    return true;
}

bool StreamPipe::is_client_connected(beast::tcp_stream& client_stream) {
    // Check if socket is open
    if (!client_stream.socket().is_open()) {
        return false;
    }

    // Try a non-blocking read to detect disconnection
    beast::error_code ec;
    client_stream.socket().non_blocking(true, ec);
    if (ec) {
        return true;  // Assume connected if we can't set non-blocking
    }

    char peek_buf[1];
    std::size_t bytes = client_stream.socket().receive(
        asio::buffer(peek_buf), tcp::socket::message_peek, ec);

    // Restore blocking mode
    beast::error_code ec2;
    client_stream.socket().non_blocking(false, ec2);

    // would_block means no data available but still connected
    if (ec == asio::error::would_block) {
        return true;
    }

    // EOF or other error means disconnected
    if (ec) {
        return false;
    }

    // bytes == 0 with no error also indicates EOF
    return bytes > 0 || !ec;
}

StreamResult StreamPipe::forward_stream(
    tcp::socket& backend_socket,
    beast::tcp_stream& client_stream,
    const http::response_header<>& response_header,
    const std::string& initial_body,
    StreamProgressCallback progress_callback)
{
    StreamResult result;
    auto start_time = std::chrono::steady_clock::now();

    spdlog::debug("StreamPipe: Starting stream forwarding");

    // First, send the HTTP response header to the client
    beast::error_code ec;

    // Build response header for client
    http::response<http::empty_body> client_response{response_header};

    // Set Transfer-Encoding: chunked for the client response
    if (config_.forward_chunked) {
        client_response.erase(http::field::content_length);
        client_response.set(http::field::transfer_encoding, "chunked");
    }

    // Set Connection header based on keep-alive
    client_response.set(http::field::connection, "keep-alive");

    // Add server header
    client_response.set(http::field::server, "NTONIX/0.1.0");

    // Serialize and send header
    http::response_serializer<http::empty_body> serializer{client_response};
    http::write_header(client_stream, serializer, ec);
    if (ec) {
        result.error_message = "Failed to write response header: " + ec.message();
        spdlog::warn("StreamPipe: {}", result.error_message);
        return result;
    }

    spdlog::debug("StreamPipe: Response header sent to client");

    // Forward any initial body data that was read with the header
    if (!initial_body.empty()) {
        std::size_t written = write_chunk_to_client(
            client_stream, initial_body.data(), initial_body.size(), ec);
        if (ec) {
            result.error_message = "Failed to write initial body: " + ec.message();
            spdlog::warn("StreamPipe: {}", result.error_message);
            return result;
        }
        result.bytes_forwarded += written;

        // Check for [DONE] marker in initial body
        if (contains_done_marker(initial_body.data(), initial_body.size())) {
            result.done_marker_received = true;
            spdlog::debug("StreamPipe: [DONE] marker found in initial body");
        }

        // Call progress callback
        if (progress_callback && !progress_callback(result.bytes_forwarded)) {
            spdlog::debug("StreamPipe: Progress callback requested stop");
            write_final_chunk(client_stream, ec);
            result.success = true;
            auto end_time = std::chrono::steady_clock::now();
            result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            return result;
        }
    }

    // If [DONE] was in initial body, we're done
    if (result.done_marker_received) {
        write_final_chunk(client_stream, ec);
        result.success = true;
        auto end_time = std::chrono::steady_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        spdlog::debug("StreamPipe: Stream complete (DONE in initial body), {} bytes in {}ms",
                      result.bytes_forwarded, result.duration.count());
        return result;
    }

    // Read from backend and forward to client in a loop
    // We're using synchronous I/O here for simplicity; the connection is dedicated to this stream
    while (true) {
        // Check if client is still connected
        if (!is_client_connected(client_stream)) {
            result.client_disconnected = true;
            spdlog::debug("StreamPipe: Client disconnected early");
            break;
        }

        // Set read timeout on backend socket
        backend_socket.non_blocking(false, ec);

        // Read from backend
        std::size_t bytes_read = backend_socket.read_some(
            asio::buffer(read_buffer_), ec);

        if (ec == asio::error::eof) {
            // Backend closed connection normally
            result.backend_closed = true;
            spdlog::debug("StreamPipe: Backend closed connection (EOF)");
            break;
        }

        if (ec) {
            if (ec == asio::error::operation_aborted) {
                spdlog::debug("StreamPipe: Operation aborted");
            } else {
                result.error_message = "Backend read error: " + ec.message();
                spdlog::warn("StreamPipe: {}", result.error_message);
            }
            break;
        }

        if (bytes_read == 0) {
            // No data but no error - continue
            continue;
        }

        // Check for [DONE] marker before forwarding
        if (contains_done_marker(read_buffer_.data(), bytes_read)) {
            result.done_marker_received = true;
            spdlog::debug("StreamPipe: [DONE] marker detected");
        }

        // Forward to client using zero-copy (const_buffer points directly to read_buffer_)
        std::size_t written = write_chunk_to_client(
            client_stream, read_buffer_.data(), bytes_read, ec);
        if (ec) {
            if (ec == asio::error::broken_pipe ||
                ec == asio::error::connection_reset) {
                result.client_disconnected = true;
                spdlog::debug("StreamPipe: Client disconnected during write");
            } else {
                result.error_message = "Client write error: " + ec.message();
                spdlog::warn("StreamPipe: {}", result.error_message);
            }
            break;
        }

        result.bytes_forwarded += written;

        // Log periodic progress
        if (result.bytes_forwarded % 65536 == 0) {
            spdlog::debug("StreamPipe: Forwarded {} bytes so far", result.bytes_forwarded);
        }

        // Call progress callback
        if (progress_callback && !progress_callback(result.bytes_forwarded)) {
            spdlog::debug("StreamPipe: Progress callback requested stop");
            break;
        }

        // If [DONE] marker was found, we can stop
        if (result.done_marker_received) {
            spdlog::debug("StreamPipe: Stopping after [DONE] marker");
            break;
        }
    }

    // Send final chunk to properly terminate chunked encoding
    write_final_chunk(client_stream, ec);

    auto end_time = std::chrono::steady_clock::now();
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    // Success if we transferred data without critical errors
    result.success = result.error_message.empty() ||
                     result.client_disconnected ||
                     result.backend_closed ||
                     result.done_marker_received;

    spdlog::info("StreamPipe: Stream complete - {} bytes in {}ms (client_disconnect={}, backend_closed={}, done={})",
                 result.bytes_forwarded, result.duration.count(),
                 result.client_disconnected, result.backend_closed, result.done_marker_received);

    return result;
}

} // namespace ntonix::proxy
