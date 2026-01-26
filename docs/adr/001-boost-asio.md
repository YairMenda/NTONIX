# ADR-001: Boost.Asio for Asynchronous I/O

## Status
Accepted

## Context
NTONIX requires handling thousands of concurrent LLM streaming connections efficiently. Each connection may remain open for extended periods while tokens are streamed from backends. A blocking I/O model would require a thread per connection, leading to excessive memory usage and context-switching overhead.

We need an asynchronous I/O framework that:
- Handles thousands of concurrent connections with minimal threads
- Provides cross-platform support (Linux, macOS, Windows)
- Integrates well with SSL/TLS for secure connections
- Has mature, production-proven implementations

## Decision
We chose **Boost.Asio** as our asynchronous I/O foundation.

### Alternatives Considered

1. **libuv** - Event loop library used by Node.js
   - Pros: Mature, cross-platform, well-documented
   - Cons: C API, no native C++ support, separate SSL integration needed

2. **libevent** - Event notification library
   - Pros: Mature, widely used
   - Cons: C API, more complex SSL integration

3. **Custom epoll/kqueue/IOCP wrapper**
   - Pros: Maximum control, no dependencies
   - Cons: Significant development effort, platform-specific code

4. **C++20 Coroutines with io_uring**
   - Pros: Modern, potentially highest performance on Linux
   - Cons: Linux-only, less mature ecosystem

### Why Boost.Asio

1. **Proactor Pattern**: Asio implements the Proactor pattern, which matches well with async operations where we initiate an operation and receive a callback on completion.

2. **Integration with Boost.Beast**: Beast provides HTTP/WebSocket support built directly on Asio, eliminating integration complexity.

3. **SSL Support**: `boost::asio::ssl` provides seamless integration with OpenSSL for TLS termination.

4. **Thread Pool Model**: The `io_context` can be run across multiple threads with automatic work distribution.

5. **Strand Support**: Strands provide serialization of handlers without explicit locking, simplifying concurrent access patterns.

6. **Industry Standard**: Asio is the basis for the Networking TS and widely used in production systems.

## Consequences

### Positive
- Single thread pool handles all I/O efficiently
- Natural integration with Beast for HTTP parsing
- Seamless SSL/TLS support
- Well-documented with extensive examples
- Cross-platform with consistent API

### Negative
- Callback-based programming can lead to "callback hell" (mitigated by structured handlers)
- Learning curve for developers unfamiliar with async patterns
- Debug complexity with async stack traces

### Trade-offs
- Using Boost adds a large dependency, but the networking components are header-only
- Asio's callback model is less elegant than coroutines, but more portable

## Implementation Notes

```cpp
// Thread pool initialization with io_context
boost::asio::io_context io_context;
std::vector<std::jthread> threads;
for (size_t i = 0; i < thread_count; ++i) {
    threads.emplace_back([&io_context] {
        io_context.run();
    });
}
```

The io_context is shared across all threads, with work automatically distributed. Each connection's handlers are associated with the same strand to ensure serialized execution without explicit locking.
