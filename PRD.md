# PRD: NTONIX - High-Performance AI Inference Gateway

## Introduction

NTONIX is a C++20 reverse proxy designed to optimize local LLM cluster infrastructure. It acts as a high-speed gateway between clients and LLM backend nodes, reducing backend load through intelligent caching, distributing requests via load balancing, and minimizing latency through zero-copy stream forwarding.

**Problem Statement**: Running multiple LLM inference requests against local backends creates redundant compute (identical prompts processed multiple times) and inefficient resource utilization (uneven load distribution). Additionally, managing SSL termination at each backend node increases complexity and overhead.

**Solution**: A single gateway that intercepts all LLM traffic, caches responses for identical prompts, distributes load evenly across backends, and handles SSL termination centrally - reducing backend compute by up to 40% while improving time-to-first-token (TTFT).

**Reference Architecture**: NGINX reverse proxy patterns adapted for AI/LLM streaming workloads.

---

## Goals

- Implement a fully functional reverse proxy with SSL termination, caching, load balancing, and stream forwarding
- Achieve zero-copy stream forwarding to minimize TTFT and memory pressure
- Support 500-1000 concurrent connections with sub-millisecond routing overhead
- Demonstrate mastery of C++20 features (std::jthread, std::shared_mutex, concepts, coroutines)
- Provide containerized deployment for easy demonstration (Docker Compose)
- Create clean, well-documented code suitable for technical interviews
- Build foundational understanding of systems programming, async I/O, and network protocols

---

## User Stories

### US-001: Project Foundation and Build System
**Description:** As a developer, I need a proper CMake build system so that the project compiles consistently across environments.

**Acceptance Criteria:**
- [x] CMakeLists.txt configured for C++20 standard
- [x] Boost.Asio and Boost.Beast dependencies properly linked
- [x] OpenSSL dependency configured for SSL/TLS support
- [x] Debug and Release build configurations working
- [x] Project compiles successfully with `cmake --build .`

- **Priority:** 1
- **Status:** true
- **Notes:** Foundation for all other components. Use modern CMake practices (target-based).

---

### US-002: Asynchronous I/O Foundation
**Description:** As a developer, I need an async I/O foundation using Boost.Asio so that the proxy can handle thousands of concurrent connections without blocking.

**Acceptance Criteria:**
- [x] io_context initialized and running on configurable thread pool
- [x] Graceful shutdown mechanism using std::jthread and stop_token
- [x] Signal handling (SIGINT, SIGTERM) for clean termination
- [x] Basic TCP acceptor listening on configured port
- [x] Connection accepted and logged without blocking other connections

- **Priority:** 1
- **Status:** true
- **Notes:** Core async foundation. All subsequent I/O builds on this.

---

### US-003: HTTP/1.1 Request Parsing
**Description:** As a proxy, I need to parse incoming HTTP/1.1 requests so that I can understand client intent and route appropriately.

**Acceptance Criteria:**
- [x] Parse HTTP method, URI, headers, and body using Boost.Beast
- [x] Extract relevant headers (Host, Content-Type, Authorization)
- [x] Handle chunked transfer encoding for request bodies
- [x] Validate request format and return 400 Bad Request for malformed requests
- [x] Support POST requests with JSON body (LLM inference payloads)

- **Priority:** 1
- **Status:** true
- **Notes:** Focus on POST /v1/chat/completions (OpenAI-compatible endpoint).

---

### US-004: Configuration System
**Description:** As an operator, I need flexible configuration so that I can customize the proxy for different environments.

**Acceptance Criteria:**
- [x] JSON configuration file parser (nlohmann/json or Boost.JSON)
- [x] Environment variable support (NTONIX_PORT, NTONIX_BACKENDS, etc.)
- [x] Command-line argument parsing (--port, --config, --help)
- [x] Configuration hierarchy: CLI > Environment > Config File > Defaults
- [x] Validate configuration and report errors clearly on startup
- [x] Hot-reload support for backend list changes (SIGHUP)

- **Priority:** 2
- **Status:** true
- **Notes:** Standard pattern used by NGINX, Redis, PostgreSQL.

**Configuration Schema:**
```json
{
  "server": {
    "port": 8080,
    "ssl_port": 8443,
    "threads": 4
  },
  "backends": [
    {"host": "localhost", "port": 8001, "weight": 1},
    {"host": "localhost", "port": 8002, "weight": 1}
  ],
  "cache": {
    "enabled": true,
    "max_size_mb": 512,
    "ttl_seconds": 3600
  },
  "ssl": {
    "cert_file": "server.crt",
    "key_file": "server.key"
  }
}
```

---

### US-005: Backend Health Monitoring
**Description:** As a proxy, I need to monitor backend health so that I only route requests to healthy nodes.

**Acceptance Criteria:**
- [x] Periodic health check pings to each backend (configurable interval)
- [x] Track backend state: healthy, unhealthy, draining
- [x] Remove unhealthy backends from load balancer pool automatically
- [x] Re-add backends to pool when health checks pass again
- [x] Log health state transitions for observability
- [x] Circuit breaker: mark backend unhealthy after N consecutive failures

- **Priority:** 2
- **Status:** true
- **Notes:** Essential for production reliability. Health endpoint: GET /health.

---

### US-006: Round-Robin Load Balancer
**Description:** As a proxy, I need to distribute requests across backends so that load is evenly spread.

**Acceptance Criteria:**
- [x] Implement weighted round-robin algorithm
- [x] Thread-safe backend selection using atomic operations
- [x] Skip unhealthy backends in rotation
- [x] Support backend weights for heterogeneous clusters
- [x] Return 503 Service Unavailable when no healthy backends exist

- **Priority:** 2
- **Status:** true
- **Notes:** Simple, predictable, easy to explain in interviews.

---

### US-007: Backend Connection Pool
**Description:** As a proxy, I need to reuse backend connections so that I avoid TCP handshake overhead for every request.

**Acceptance Criteria:**
- [x] Maintain pool of persistent connections per backend
- [x] Configurable pool size per backend (default: 10)
- [x] Connection reuse with keep-alive
- [x] Automatic connection cleanup for idle/stale connections
- [x] Create new connection if pool exhausted (up to max limit)
- [x] Thread-safe connection checkout/checkin

- **Priority:** 3
- **Status:** true
- **Notes:** Significant performance optimization. Use RAII for connection lifecycle.

---

### US-008: Request Forwarding to Backend
**Description:** As a proxy, I need to forward client requests to selected backends so that LLM inference can be performed.

**Acceptance Criteria:**
- [x] Forward complete HTTP request to backend
- [x] Add/modify headers: X-Forwarded-For, X-Real-IP, X-Request-ID
- [x] Handle backend connection errors gracefully (retry or fail fast)
- [x] Timeout handling for backend responses (configurable)
- [x] Pass through all relevant headers to backend

- **Priority:** 2
- **Status:** true
- **Notes:** Core proxy functionality.

---

### US-009: Zero-Copy Stream Forwarding
**Description:** As a proxy, I need to forward LLM response streams to clients without buffering so that TTFT is minimized.

**Acceptance Criteria:**
- [x] Forward SSE (Server-Sent Events) chunks as they arrive from backend
- [x] Use asio::const_buffer for zero-copy forwarding
- [x] Never buffer entire response in memory
- [x] Handle chunked transfer encoding from backend
- [x] Maintain streaming until backend closes connection or sends [DONE]
- [x] Client disconnect detection to stop backend streaming early

- **Priority:** 1
- **Status:** true
- **Notes:** Key differentiator. Critical for interview discussions on memory efficiency.

---

### US-010: Thread-Safe LRU Cache
**Description:** As a proxy, I need to cache LLM responses so that identical prompts don't require redundant inference.

**Acceptance Criteria:**
- [x] Hash prompt content to create cache key (SHA-256 or XXHash)
- [x] Store complete response with metadata (timestamp, size, hit count)
- [x] LRU eviction when cache exceeds configured size
- [x] std::shared_mutex for concurrent read access
- [x] Configurable TTL for cache entries
- [x] Cache bypass for requests with Cache-Control: no-cache
- [x] Cache statistics endpoint (hit rate, size, entries)

- **Priority:** 2
- **Status:** true
- **Notes:** Demonstrate understanding of reader-writer locks and cache invalidation.

---

### US-011: SSL/TLS Termination
**Description:** As a secure gateway, I need to handle SSL/TLS so that client connections are encrypted.

**Acceptance Criteria:**
- [x] Load SSL certificate and private key from files
- [x] Accept HTTPS connections on configured SSL port
- [x] TLS 1.2/1.3 support via OpenSSL
- [x] Forward decrypted requests to backends over plain HTTP
- [x] Graceful handling of SSL handshake failures
- [x] Support for SNI (Server Name Indication)

- **Priority:** 2
- **Status:** true
- **Notes:** Production requirement. Use boost::asio::ssl.

---

### US-012: Logging and Observability
**Description:** As an operator, I need structured logging so that I can monitor and debug the proxy.

**Acceptance Criteria:**
- [x] Structured logging with levels (DEBUG, INFO, WARN, ERROR)
- [x] Log format: timestamp, level, component, message, context
- [x] Access log: method, path, status, latency, cache hit/miss
- [x] Configurable log level via config/environment
- [x] Log rotation support (or stdout for container deployment)
- [x] Request tracing with X-Request-ID propagation

- **Priority:** 3
- **Status:** true
- **Notes:** Use spdlog or similar. Essential for debugging.

---

### US-013: Metrics Endpoint
**Description:** As an operator, I need a metrics endpoint so that I can monitor proxy performance.

**Acceptance Criteria:**
- [x] GET /metrics endpoint returning JSON statistics
- [x] Metrics: requests_total, requests_active, cache_hits, cache_misses
- [x] Per-backend metrics: requests, errors, latency_avg
- [x] System metrics: uptime, memory_usage, connections_active
- [x] Thread-safe metric collection using atomics

- **Priority:** 3
- **Status:** true
- **Notes:** Prometheus-compatible format is a bonus.

---

### US-014: Docker Containerization
**Description:** As a developer, I need Docker support so that the project is easy to build and demo.

**Acceptance Criteria:**
- [x] Multi-stage Dockerfile (build + runtime)
- [x] Docker Compose with proxy + 2 mock LLM backends
- [x] Mock LLM backend (Python FastAPI) with configurable delay
- [x] Mock backend supports streaming responses (SSE)
- [x] Single command startup: `docker-compose up`
- [x] Health checks configured in compose file

- **Priority:** 2
- **Status:** true
- **Notes:** Critical for interviews. "Run with one command" is impressive.

---

### US-015: Integration Tests
**Description:** As a developer, I need integration tests so that I can verify end-to-end functionality.

**Acceptance Criteria:**
- [x] Test: Request forwarding to single backend
- [x] Test: Load balancing distributes across multiple backends
- [x] Test: Cache hit returns cached response
- [x] Test: Unhealthy backend is skipped
- [x] Test: SSL termination works correctly
- [x] Test: Streaming responses forwarded correctly
- [x] Tests run in CI-compatible manner

- **Priority:** 3
- **Status:** true
- **Notes:** Use pytest for integration tests against running proxy.

---

### US-016: Documentation and README
**Description:** As a reviewer, I need clear documentation so that I can understand and run the project.

**Acceptance Criteria:**
- [x] README with project overview and architecture diagram
- [x] Quick start guide (build + run in <5 minutes)
- [x] Configuration reference with all options documented
- [x] API documentation for proxy endpoints
- [x] Architecture decision records (ADRs) for key choices
- [x] Code comments explaining non-obvious design decisions

- **Priority:** 3
- **Status:** true
- **Notes:** Portfolio quality. First impression matters.

---

## Functional Requirements

- **FR-01:** The proxy MUST accept HTTP/1.1 connections on a configurable port
- **FR-02:** The proxy MUST accept HTTPS connections with TLS 1.2+ on a configurable SSL port
- **FR-03:** The proxy MUST forward requests to healthy backend nodes using round-robin selection
- **FR-04:** The proxy MUST cache responses keyed by prompt hash with configurable TTL
- **FR-05:** The proxy MUST forward streaming responses (SSE) without buffering entire response
- **FR-06:** The proxy MUST health-check backends periodically and exclude unhealthy nodes
- **FR-07:** The proxy MUST implement circuit breaker pattern for failing backends
- **FR-08:** The proxy MUST support configuration via file, environment variables, and CLI args
- **FR-09:** The proxy MUST handle graceful shutdown without dropping active connections
- **FR-10:** The proxy MUST log all requests with method, path, status, and latency
- **FR-11:** The proxy MUST expose /metrics endpoint with operational statistics
- **FR-12:** The proxy MUST expose /health endpoint for its own health status
- **FR-13:** The proxy MUST add X-Forwarded-For and X-Request-ID headers to forwarded requests
- **FR-14:** The proxy MUST return 503 when no healthy backends are available
- **FR-15:** The proxy MUST return 504 when backend response times out

---

## Non-Goals (Out of Scope)

- **HTTP/2 or HTTP/3 support** - HTTP/1.1 is sufficient for MVP
- **WebSocket proxying** - Focus on HTTP request/response and SSE streaming
- **Authentication/Authorization** - Pass through auth headers; don't implement auth logic
- **Rate limiting** - Can be added later; not core to the demo
- **Request/response transformation** - Pure proxy, no body modification
- **Multi-region deployment** - Local/single-region only
- **Kubernetes operator** - Docker Compose is sufficient
- **GUI dashboard** - CLI and metrics endpoint are sufficient
- **Least-connections algorithm** - Round-robin only for MVP simplicity
- **Dynamic backend discovery** - Static configuration only

---

## Technical Considerations

### Dependencies
| Library | Version | Purpose |
|---------|---------|---------|
| Boost.Asio | 1.83+ | Async I/O, networking |
| Boost.Beast | 1.83+ | HTTP parsing, WebSocket |
| OpenSSL | 3.0+ | TLS/SSL support |
| nlohmann/json | 3.11+ | JSON parsing |
| spdlog | 1.12+ | Structured logging |
| xxHash | 0.8+ | Fast hashing for cache keys |

### C++20 Features to Demonstrate
- `std::jthread` with stop_token for graceful shutdown
- `std::shared_mutex` for reader-writer locking in cache
- `std::atomic` for lock-free counters and metrics
- Concepts for type constraints on templates
- `std::format` for string formatting (if compiler supports)
- Designated initializers for configuration structs

### Architecture Patterns
- **Proactor pattern** via Boost.Asio for async I/O
- **Connection pooling** for backend efficiency
- **Circuit breaker** for fault tolerance
- **RAII** throughout for resource management

### Thread Model
```
Main Thread: Signal handling, configuration reload
I/O Thread Pool (N threads): All async operations
  - Accept connections
  - Read/write sockets
  - Timer callbacks for health checks
```

### Memory Considerations
- No dynamic allocation in hot path (connection handling)
- Pre-allocated buffers for request/response parsing
- Zero-copy forwarding using asio::const_buffer
- Cache size bounded by configuration

---

## Design Considerations

### Project Structure
```
ntonix/
├── CMakeLists.txt
├── Dockerfile
├── docker-compose.yml
├── config/
│   └── ntonix.json
├── src/
│   ├── main.cpp
│   ├── server/
│   │   ├── server.hpp/cpp        # Main server class
│   │   ├── connection.hpp/cpp    # Client connection handler
│   │   └── ssl_context.hpp/cpp   # SSL configuration
│   ├── proxy/
│   │   ├── forwarder.hpp/cpp     # Request forwarding logic
│   │   ├── stream_pipe.hpp/cpp   # Zero-copy streaming
│   │   └── backend_pool.hpp/cpp  # Connection pool
│   ├── balancer/
│   │   ├── load_balancer.hpp/cpp # Round-robin implementation
│   │   ├── backend.hpp/cpp       # Backend node representation
│   │   └── health_checker.hpp/cpp
│   ├── cache/
│   │   ├── lru_cache.hpp/cpp     # Thread-safe LRU cache
│   │   └── cache_key.hpp/cpp     # Prompt hashing
│   ├── config/
│   │   └── config.hpp/cpp        # Configuration loading
│   └── util/
│       ├── logger.hpp/cpp
│       └── metrics.hpp/cpp
├── tests/
│   └── integration/
├── mock/
│   └── llm_backend.py            # FastAPI mock server
└── docs/
    └── architecture.md
```

---

## Success Metrics

- **Build & Run**: Project builds and runs with `docker-compose up` in <2 minutes
- **Demo Flow**: Can demonstrate cache hit, load balancing, and failover in <5 minutes
- **Code Quality**: No compiler warnings, consistent style, clear naming
- **Performance**: Handle 500 concurrent connections with <10ms p99 routing latency
- **Cache Effectiveness**: Achieve >30% cache hit rate with repeated prompts
- **Reliability**: No crashes or memory leaks under load testing
- **Interview Ready**: Can explain any component's design decisions confidently

---

## Open Questions

1. **Streaming cache**: Should we cache streaming responses? (Complex: need to store chunks and replay)
   - *Initial decision*: Cache only complete, non-streaming responses for MVP

2. **Cache key granularity**: Should temperature/model parameters affect cache key?
   - *Initial decision*: Yes, include model and temperature in hash

3. **Backend protocol**: HTTP only or support HTTPS to backends?
   - *Initial decision*: HTTP only to backends (proxy handles SSL termination)

4. **Graceful shutdown timeout**: How long to wait for active connections?
   - *Initial decision*: 30 seconds configurable

5. **Health check endpoint format**: What should backends expose?
   - *Initial decision*: GET /health returning 200 OK

---

## Implementation Order (Recommended)

**Phase 1: Foundation**
1. US-001: Build System
2. US-002: Async I/O Foundation
3. US-003: HTTP Parsing

**Phase 2: Core Proxy**
4. US-004: Configuration
5. US-008: Request Forwarding
6. US-006: Load Balancer
7. US-009: Zero-Copy Streaming

**Phase 3: Reliability**
8. US-005: Health Monitoring
9. US-007: Connection Pool
10. US-010: LRU Cache

**Phase 4: Production Ready**
11. US-011: SSL/TLS
12. US-012: Logging
13. US-013: Metrics

**Phase 5: Demo Ready**
14. US-014: Docker
15. US-015: Integration Tests
16. US-016: Documentation

---

*Generated following PRD best practices. Each user story is sized for implementation in one focused session.*
