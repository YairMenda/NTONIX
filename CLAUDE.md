# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**NTONIX** is a high-performance AI inference gateway - a C++20 reverse proxy designed to optimize local LLM cluster infrastructure. It acts as a high-speed gateway that reduces backend load through intelligent request handling, caching, and asynchronous stream forwarding.

**Value Proposition**: Reduces LLM inference costs and latency by eliminating redundant compute through caching (targeting 40% reduction in backend load) and minimizing time-to-first-token through zero-copy stream forwarding.

## Architecture

### Core Components

1. **Asynchronous Foundation**: Built on Boost.Asio's io_context for non-blocking I/O, enabling thousands of concurrent AI streams with minimal thread overhead

2. **Layer-7 Load Balancing**: Intelligent request distribution across LLM backend nodes with health monitoring and circuit-breaking for failed nodes (Weighted Round-Robin or Least-Connections)

3. **Zero-Copy Stream Forwarding**: Forwards LLM token chunks directly to client sockets using asio::const_buffer without buffering entire responses - minimizes memory pressure and reduces TTFT

4. **Thread-Safe LRU Cache**: Custom cache mapping prompt hashes to responses using std::shared_mutex for concurrent reads with minimal lock contention

5. **SSL/TLS Termination**: Acts as secure entry point, decrypting traffic via OpenSSL before forwarding to backend nodes

## Tech Stack

- **Language**: C++20 (std::jthread, std::shared_mutex, Concepts)
- **Networking**: Boost.Asio (async I/O), Boost.Beast (HTTP/1.1)
- **Security**: OpenSSL via boost::asio::ssl
- **Build System**: CMake
- **Containerization**: Docker (for multi-node LLM backend simulation)
- **Test Mocks**: Python FastAPI/Flask (simulating LLM streaming with delays)

## Development Commands

### Building
```bash
mkdir build && cd build
cmake ..
cmake --build .
```

### Running
The proxy will listen for client connections and forward to configured LLM backend nodes. Backend configuration and port settings should be specified in a config file or environment variables.

### Testing
Use Docker to spin up mock LLM backends, then send test requests through the proxy to verify:
- Load balancing across backends
- Cache hit/miss behavior
- SSL termination
- Stream forwarding performance
- Circuit breaker activation on backend failures

## Key Implementation Notes

- **Asynchronous Patterns**: All I/O operations use Boost.Asio async calls to prevent blocking
- **Memory Efficiency**: Never buffer entire LLM responses - forward chunks as they arrive
- **Thread Safety**: Cache operations use shared_mutex for reader-writer locking patterns
- **Error Handling**: Implement circuit breakers to detect and isolate failing backend nodes
- **Performance**: Minimize allocations in hot paths, use const_buffer for zero-copy forwarding

## C++ Design Patterns and Best Practices

### LRU Cache (`src/cache/lru_cache.hpp`)

**Thread Safety Model**: The `CacheEntry` struct uses regular `std::uint64_t` for `hit_count` instead of `std::atomic<std::uint64_t>`. This is intentional because:
- `CacheEntry` must be copyable/movable for use in `std::list<Node>`
- `std::atomic` types are non-copyable and non-movable (deleted copy/move constructors)
- Thread safety is already provided by the `LruCache`'s `std::shared_mutex` - all entry access is protected
- The atomic counters at the `LruCache` level (`hits_`, `misses_`, etc.) are for lock-free statistics reads

### Logger Singleton (`src/util/logger.hpp`)

**Singleton with `unique_ptr`**: The `Logger` class uses a public destructor with private constructor pattern:
- Constructor is private to prevent external instantiation (singleton pattern)
- Destructor is public to allow `std::unique_ptr<Logger>` to clean up the singleton
- `std::unique_ptr` uses `std::default_delete` which requires public destructor access
- This is the recommended pattern for singletons managed by smart pointers

### General Guidelines

- **Avoid `std::atomic` in copyable/movable structs**: If a struct needs to be stored in STL containers or returned by value, don't use `std::atomic` members. Use external synchronization instead.
- **Smart pointer singletons**: When using `std::unique_ptr` for singleton storage, ensure the destructor is accessible (public or via friend deleter).
- **Prefer `std::shared_mutex`**: For read-heavy workloads (like cache lookups), use `std::shared_mutex` with `std::shared_lock` for reads and `std::unique_lock` for writes.
