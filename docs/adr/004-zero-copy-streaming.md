# ADR-004: Zero-Copy Stream Forwarding

## Status
Accepted

## Context
LLM inference produces streaming responses where tokens are generated one at a time. A typical response might contain hundreds of tokens streamed over several seconds. The proxy must:
- Minimize Time to First Token (TTFT) latency
- Avoid buffering entire responses in memory
- Handle thousands of concurrent streams efficiently
- Support Server-Sent Events (SSE) format

## Decision
We implement **zero-copy stream forwarding** using `asio::const_buffer` to forward chunks directly from backend to client without intermediate copies.

### Alternatives Considered

1. **Full Response Buffering**
   - Pros: Simple implementation, easy caching
   - Cons: High memory usage, adds latency, defeats streaming purpose

2. **Copy-based Forwarding**
   - Pros: Simpler memory management
   - Cons: Unnecessary allocations, increased latency

3. **Memory-mapped Buffers**
   - Pros: OS-level optimization
   - Cons: Complex for streaming data, overkill

### Why Zero-Copy

Zero-copy forwarding eliminates memory allocation and copying in the hot path:

```
Backend -> [recv buffer] -> const_buffer view -> [send to client]
                              ^
                              No copy here
```

## Implementation

### Stream Pipe Design

```cpp
class StreamPipe {
public:
    // Forward chunks as they arrive
    void forward_chunk(asio::const_buffer chunk) {
        // Create a view of the received data
        // No copy - just pointer + size
        asio::async_write(client_socket_, chunk,
            [this](error_code ec, size_t bytes) {
                if (!ec) {
                    // Chunk forwarded, ready for next
                    start_backend_read();
                }
            });
    }

private:
    std::array<char, 8192> read_buffer_;  // Reused for each chunk
    tcp::socket& client_socket_;
    tcp::socket& backend_socket_;
};
```

### SSE Format Handling

LLM streaming typically uses Server-Sent Events:
```
data: {"token": "Hello"}

data: {"token": " world"}

data: [DONE]
```

The proxy forwards these chunks verbatim without parsing the content.

## Memory Model

### Per-Connection Buffers
Each connection maintains a fixed-size read buffer (default 8KB):
```cpp
std::array<char, 8192> buffer_;  // Stack-allocated, reused
```

Benefits:
- No dynamic allocation per chunk
- Predictable memory usage: connections × buffer_size
- Cache-friendly sequential access

### Buffer Lifetime
The buffer remains valid until the async write completes:
```cpp
// Safe: buffer_ valid until handler called
asio::async_read(backend_, asio::buffer(buffer_),
    [this](error_code ec, size_t bytes) {
        // buffer_[0..bytes] is valid here
        forward_chunk(asio::buffer(buffer_, bytes));
    });
```

## Consequences

### Positive
- Minimal TTFT - first token forwarded immediately
- Constant memory per connection regardless of response size
- No heap allocations in steady state
- Scales to thousands of concurrent streams

### Negative
- Cannot cache streaming responses (trade-off accepted)
- Buffer size limits maximum chunk size
- Requires careful lifetime management

### Performance Characteristics
- **TTFT**: Network RTT only (no processing delay)
- **Memory**: O(connections × buffer_size), not O(connections × response_size)
- **Throughput**: Limited by network, not copying

## Client Disconnect Handling

When a client disconnects mid-stream:
1. Detect write failure to client socket
2. Cancel pending backend read
3. Close backend connection (signals backend to stop generation)

This prevents wasted inference compute when clients abandon requests.

## Metrics
Stream forwarding metrics:
- `streaming_active`: Current streaming connections
- `streaming_bytes_forwarded`: Total bytes forwarded
- `streaming_errors`: Forwarding failures
