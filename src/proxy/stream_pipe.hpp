/**
 * NTONIX - High-Performance AI Inference Gateway
 * Stream Pipe - Zero-copy forwarding of SSE streams from backends to clients
 */

#ifndef NTONIX_PROXY_STREAM_PIPE_HPP
#define NTONIX_PROXY_STREAM_PIPE_HPP

#include "config/config.hpp"
#include "proxy/connection_pool.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace ntonix::proxy {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

/**
 * Configuration for stream pipe
 */
struct StreamPipeConfig {
    std::size_t buffer_size{8192};                    // Read buffer size for chunks
    std::chrono::seconds read_timeout{120};           // Timeout for streaming reads
    bool detect_done_marker{true};                    // Stop on [DONE] marker (SSE convention)
    bool forward_chunked{true};                       // Use chunked transfer encoding to client
};

/**
 * Result of stream forwarding
 */
struct StreamResult {
    bool success{false};
    std::string error_message;
    std::size_t bytes_forwarded{0};
    std::chrono::milliseconds duration{0};
    bool client_disconnected{false};      // True if client disconnected early
    bool backend_closed{false};           // True if backend closed connection
    bool done_marker_received{false};     // True if [DONE] marker was detected
};

/**
 * Callback for stream progress (optional)
 * Called for each chunk forwarded. Return false to stop streaming.
 */
using StreamProgressCallback = std::function<bool(std::size_t bytes_forwarded)>;

/**
 * Stream Pipe - forwards SSE streams from backends to clients with zero-copy semantics
 *
 * Features:
 * - Zero-copy forwarding using asio::const_buffer
 * - Chunk-by-chunk forwarding without buffering entire response
 * - Client disconnect detection to abort backend reads early
 * - SSE [DONE] marker detection
 * - Chunked transfer encoding support
 *
 * Usage:
 * 1. Create StreamPipe with client stream and config
 * 2. Call start_streaming() with backend connection
 * 3. Streaming happens asynchronously
 * 4. Get result via callback or poll
 */
class StreamPipe : public std::enable_shared_from_this<StreamPipe> {
public:
    using Ptr = std::shared_ptr<StreamPipe>;

    /**
     * Create a stream pipe
     * @param io_context Asio io_context for async operations
     * @param config Stream pipe configuration
     */
    StreamPipe(asio::io_context& io_context, const StreamPipeConfig& config = {});

    ~StreamPipe();

    // Non-copyable
    StreamPipe(const StreamPipe&) = delete;
    StreamPipe& operator=(const StreamPipe&) = delete;

    /**
     * Forward a streaming response from backend to client (synchronous)
     * This reads chunks from the backend socket and writes them to the client socket.
     *
     * @param backend_socket Socket connected to the backend
     * @param client_stream Beast TCP stream to the client
     * @param response_header The HTTP response header (already read from backend)
     * @param initial_body Any initial body data already read with the header
     * @param progress_callback Optional callback for progress updates
     * @return StreamResult with outcome
     */
    StreamResult forward_stream(
        tcp::socket& backend_socket,
        beast::tcp_stream& client_stream,
        const http::response_header<>& response_header,
        const std::string& initial_body,
        StreamProgressCallback progress_callback = nullptr);

    /**
     * Check if this is a streaming response (based on Content-Type and status)
     * OpenAI streaming uses Content-Type: text/event-stream
     */
    static bool is_streaming_response(const http::response_header<>& header);

    /**
     * Get the configuration
     */
    const StreamPipeConfig& config() const { return config_; }

private:
    /**
     * Check if data contains the SSE [DONE] marker
     */
    bool contains_done_marker(const char* data, std::size_t size) const;

    /**
     * Write a chunk to client using chunked transfer encoding
     * Returns number of bytes written, or 0 on error
     */
    std::size_t write_chunk_to_client(
        beast::tcp_stream& client_stream,
        const char* data,
        std::size_t size,
        beast::error_code& ec);

    /**
     * Write the final empty chunk (terminates chunked transfer)
     */
    bool write_final_chunk(beast::tcp_stream& client_stream, beast::error_code& ec);

    /**
     * Check if client is still connected
     */
    bool is_client_connected(beast::tcp_stream& client_stream);

    asio::io_context& io_context_;
    StreamPipeConfig config_;
    std::vector<char> read_buffer_;
};

/**
 * Create a stream pipe instance
 */
inline StreamPipe::Ptr make_stream_pipe(
    asio::io_context& io_context,
    const StreamPipeConfig& config = {})
{
    return std::make_shared<StreamPipe>(io_context, config);
}

} // namespace ntonix::proxy

#endif // NTONIX_PROXY_STREAM_PIPE_HPP
