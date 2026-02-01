# NTONIX - High-Performance AI Inference Gateway

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Boost](https://img.shields.io/badge/Boost-1.83%2B-orange.svg)](https://www.boost.org/)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

**NTONIX** is a production-grade C++20 reverse proxy designed to optimize local LLM (Large Language Model) cluster infrastructure. It acts as a high-speed gateway between clients and LLM backend nodes, reducing backend load by up to 40% through intelligent caching, distributing requests via load balancing, and minimizing latency through zero-copy stream forwarding.

## 🎯 Problem Statement

Running multiple LLM inference requests against local backends creates:
- **Redundant compute**: Identical prompts processed multiple times
- **Inefficient resource utilization**: Uneven load distribution across nodes
- **SSL complexity**: Managing SSL termination at each backend increases overhead

## ✨ Solution

NTONIX provides a single gateway that:
- **Caches responses** for identical prompts, eliminating redundant inference
- **Distributes load** evenly across healthy backend nodes using weighted round-robin
- **Handles SSL termination** centrally, simplifying backend configuration
- **Forwards streams** with zero-copy to minimize time-to-first-token (TTFT)

## 🏗️ Architecture

```
┌─────────────┐
│   Client    │
│  (Browser/   │
│   App)      │
└──────┬──────┘
       │ HTTPS/HTTP
       │
       ▼
┌─────────────────────────────────────────────────┐
│              NTONIX Gateway                      │
│  ┌──────────────────────────────────────────┐  │
│  │  SSL Termination (OpenSSL)                │  │
│  └──────────────────────────────────────────┘  │
│  ┌──────────────────────────────────────────┐  │
│  │  HTTP/1.1 Parser (Boost.Beast)           │  │
│  └──────────────────────────────────────────┘  │
│  ┌──────────────────────────────────────────┐  │
│  │  LRU Cache (Thread-Safe)                 │  │
│  │  • Prompt hash → Response mapping        │  │
│  │  • Shared mutex for concurrent reads     │  │
│  └──────────────────────────────────────────┘  │
│  ┌──────────────────────────────────────────┐  │
│  │  Load Balancer (Weighted Round-Robin)    │  │
│  │  • Health monitoring                     │  │
│  │  • Circuit breaker                      │  │
│  └──────────────────────────────────────────┘  │
│  ┌──────────────────────────────────────────┐  │
│  │  Connection Pool (Per Backend)            │  │
│  │  • Reuse TCP connections                 │  │
│  │  • Keep-alive support                    │  │
│  └──────────────────────────────────────────┘  │
│  ┌──────────────────────────────────────────┐  │
│  │  Zero-Copy Stream Forwarder              │  │
│  │  • SSE chunk forwarding                  │  │
│  │  • asio::const_buffer (no buffering)    │  │
│  └──────────────────────────────────────────┘  │
└─────────────────────────────────────────────────┘
       │ HTTP (Plain)
       │
       ├──────────────┬──────────────┐
       ▼              ▼              ▼
┌──────────┐   ┌──────────┐   ┌──────────┐
│ Backend  │   │ Backend  │   │ Backend  │
│   #1     │   │   #2     │   │   #N     │
│ (LLM)    │   │ (LLM)    │   │ (LLM)    │
└──────────┘   └──────────┘   └──────────┘
```

### Core Components

1. **Asynchronous I/O Foundation**: Built on Boost.Asio's `io_context` for non-blocking I/O, enabling thousands of concurrent connections with minimal thread overhead
2. **Layer-7 Load Balancing**: Weighted round-robin with health monitoring and circuit-breaking
3. **Zero-Copy Stream Forwarding**: Forwards LLM token chunks directly using `asio::const_buffer` without buffering
4. **Thread-Safe LRU Cache**: Custom cache using `std::shared_mutex` for concurrent reads
5. **SSL/TLS Termination**: Centralized SSL handling via OpenSSL

### Thread Model

```
Main Thread: Signal handling, configuration reload
I/O Thread Pool (N threads): All async operations
  - Accept connections
  - Read/write sockets
  - Timer callbacks for health checks
```

## 🚀 Quick Start

### Prerequisites

- **C++20 compiler** (GCC 10+, Clang 12+, or MSVC 2019+)
- **CMake 3.20+**
- **Boost 1.74+** (1.83+ preferred)
- **OpenSSL 3.0+**
- **vcpkg** (optional, for dependency management)

### Build from Source

```bash
# Clone the repository
git clone <repository-url>
cd NTONIX

# Create build directory
mkdir build && cd build

# Configure with CMake
cmake ..

# Build
cmake --build . --config Release

# The executable will be in build/src/ntonix (or build/src/Release/ntonix on Windows)
```

### Run with Docker Compose (Recommended)

The fastest way to get started:

```bash
# Start proxy + 2 mock LLM backends
docker-compose up

# In another terminal, test the proxy
curl http://localhost:8080/health
```

This starts:
- **NTONIX proxy** on port 8080 (HTTP)
- **2 mock LLM backends** (Python FastAPI) on ports 8001 and 8002

### Run Locally

```bash
# From build directory
./src/ntonix --config ../config/ntonix.json

# Or with environment variables
NTONIX_PORT=8080 ./src/ntonix
```

## ⚙️ Configuration

NTONIX supports configuration via **file**, **environment variables**, and **command-line arguments** with the following precedence:

**CLI Arguments > Environment Variables > Config File > Defaults**

### Configuration File

Create `config/ntonix.json`:

```json
{
  "server": {
    "port": 8080,
    "ssl_port": 8443,
    "threads": 4,
    "bind_address": "0.0.0.0"
  },
  "backends": [
    {"host": "localhost", "port": 8001, "weight": 1},
    {"host": "localhost", "port": 8002, "weight": 2}
  ],
  "cache": {
    "enabled": true,
    "max_size_mb": 512,
    "ttl_seconds": 3600
  },
  "ssl": {
    "enabled": false,
    "cert_file": "server.crt",
    "key_file": "server.key"
  },
  "logging": {
    "level": "info",
    "file": "",
    "max_file_size_mb": 100,
    "max_files": 5,
    "enable_console": true,
    "enable_colors": true
  }
}
```

### Configuration Options

#### Server Settings

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `server.port` | integer | 8080 | HTTP listening port |
| `server.ssl_port` | integer | 8443 | HTTPS listening port |
| `server.threads` | integer | CPU cores | Number of I/O threads |
| `server.bind_address` | string | "0.0.0.0" | IP address to bind to |

#### Backend Configuration

| Option | Type | Description |
|--------|------|-------------|
| `backends[].host` | string | Backend hostname or IP |
| `backends[].port` | integer | Backend port |
| `backends[].weight` | integer | Load balancing weight (higher = more traffic) |

#### Cache Settings

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `cache.enabled` | boolean | true | Enable response caching |
| `cache.max_size_mb` | integer | 512 | Maximum cache size in MB |
| `cache.ttl_seconds` | integer | 3600 | Time-to-live for cache entries |

#### SSL/TLS Settings

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `ssl.enabled` | boolean | false | Enable SSL/TLS termination |
| `ssl.cert_file` | string | "server.crt" | Path to SSL certificate |
| `ssl.key_file` | string | "server.key" | Path to SSL private key |

#### Logging Settings

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `logging.level` | string | "info" | Log level: trace, debug, info, warn, error |
| `logging.file` | string | "" | Log file path (empty = stdout only) |
| `logging.max_file_size_mb` | integer | 100 | Maximum log file size before rotation |
| `logging.max_files` | integer | 5 | Number of rotated log files to keep |
| `logging.enable_console` | boolean | true | Enable console output |
| `logging.enable_colors` | boolean | true | Enable colored output |

### Environment Variables

```bash
export NTONIX_PORT=8080
export NTONIX_SSL_PORT=8443
export NTONIX_THREADS=4
export NTONIX_CONFIG=config/ntonix.json
```

### Command-Line Arguments

```bash
./ntonix --help
./ntonix --port 8080 --config config/ntonix.json
./ntonix --port 8080 --backend localhost:8001 --backend localhost:8002
```

### Configuration Reload

Send `SIGHUP` to reload backend configuration without restart:

```bash
kill -HUP <pid>
```

Backend list changes are applied immediately; other settings require a restart.

## 📡 API Documentation

### Health Check

Check proxy health status.

```http
GET /health
```

**Response:**
```json
{
  "status": "healthy"
}
```

**Status Codes:**
- `200 OK`: Proxy is healthy

---

### Metrics

Get operational statistics and performance metrics.

```http
GET /metrics
```

**Response:**
```json
{
  "requests_total": 1234,
  "requests_active": 5,
  "requests_success": 1200,
  "requests_error": 34,
  "cache_hits": 456,
  "cache_misses": 778,
  "cache_hit_rate": 0.3695,
  "uptime_seconds": 3600,
  "connections_active": 10,
  "connections_total": 5000,
  "memory_cache_bytes": 52428800,
  "backends": [
    {
      "host": "localhost",
      "port": 8001,
      "requests": 600,
      "errors": 10,
      "latency_avg_ms": 125.5,
      "error_rate": 0.0167
    }
  ]
}
```

**Status Codes:**
- `200 OK`: Metrics retrieved successfully

---

### Cache Statistics

Get cache performance statistics.

```http
GET /cache/stats
```

**Response:**
```json
{
  "enabled": true,
  "hits": 456,
  "misses": 778,
  "hit_rate": 0.3695,
  "evictions": 12,
  "expired": 5,
  "entries": 123,
  "size_bytes": 52428800,
  "max_size_bytes": 536870912
}
```

**Status Codes:**
- `200 OK`: Statistics retrieved successfully

---

### Chat Completions (OpenAI-Compatible)

Proxy LLM inference requests to backend nodes. Supports both streaming and non-streaming modes.

#### Non-Streaming Request

```http
POST /v1/chat/completions
Content-Type: application/json

{
  "model": "llama2",
  "messages": [
    {"role": "user", "content": "Hello, how are you?"}
  ],
  "stream": false,
  "temperature": 0.7
}
```

**Response Headers:**
- `X-Cache`: `HIT` or `MISS`
- `X-Request-ID`: Unique request identifier

**Response:**
```json
{
  "id": "chatcmpl-123",
  "object": "chat.completion",
  "created": 1677652288,
  "model": "llama2",
  "choices": [
    {
      "index": 0,
      "message": {
        "role": "assistant",
        "content": "Hello! I'm doing well, thank you for asking."
      },
      "finish_reason": "stop"
    }
  ],
  "usage": {
    "prompt_tokens": 9,
    "completion_tokens": 12,
    "total_tokens": 21
  }
}
```

#### Streaming Request

```http
POST /v1/chat/completions
Content-Type: application/json

{
  "model": "llama2",
  "messages": [
    {"role": "user", "content": "Tell me a story"}
  ],
  "stream": true
}
```

**Response:** Server-Sent Events (SSE) stream:

```
data: {"id":"chatcmpl-123","object":"chat.completion.chunk","created":1677652288,"model":"llama2","choices":[{"index":0,"delta":{"content":"Once"},"finish_reason":null}]}

data: {"id":"chatcmpl-123","object":"chat.completion.chunk","created":1677652288,"model":"llama2","choices":[{"index":0,"delta":{"content":" upon"},"finish_reason":null}]}

data: [DONE]
```

**Status Codes:**
- `200 OK`: Request forwarded successfully
- `400 Bad Request`: Malformed request
- `415 Unsupported Media Type`: Content-Type must be `application/json`
- `503 Service Unavailable`: No healthy backends available
- `504 Gateway Timeout`: Backend response timeout

**Cache Behavior:**
- Non-streaming responses are cached (if cache enabled)
- Streaming responses are not cached
- Cache can be bypassed with `Cache-Control: no-cache` header

---

### Root Endpoint

Get gateway information and available endpoints.

```http
GET /
```

**Response:**
```json
{
  "name": "NTONIX",
  "version": "0.1.0",
  "description": "High-Performance AI Inference Gateway",
  "endpoints": {
    "health": "/health",
    "metrics": "/metrics",
    "cache_stats": "/cache/stats",
    "chat_completions": "/v1/chat/completions"
  }
}
```

---

## 🔧 Architecture Decision Records (ADRs)

Key architectural decisions are documented in `docs/adr/`:

- **[ADR-001: Boost.Asio for Async I/O](docs/adr/001-boost-asio.md)** - Why Boost.Asio over raw sockets
- **[ADR-002: SWRR Load Balancing](docs/adr/002-swrr-load-balancing.md)** - Weighted round-robin algorithm choice
- **[ADR-003: LRU Cache Design](docs/adr/003-lru-cache-design.md)** - Thread-safe cache implementation
- **[ADR-004: Zero-Copy Streaming](docs/adr/004-zero-copy-streaming.md)** - Memory-efficient stream forwarding
- **[ADR-005: Configuration Hierarchy](docs/adr/005-configuration-hierarchy.md)** - CLI > Env > File > Defaults

## 🧪 Testing

### Integration Tests

Run integration tests against a running proxy:

```bash
# Start proxy and backends
docker-compose up -d

# Run tests
cd tests/integration
pip install -r requirements.txt
pytest

# Stop services
docker-compose down
```

### Manual Testing

```bash
# Health check
curl http://localhost:8080/health

# Metrics
curl http://localhost:8080/metrics | jq

# Cache stats
curl http://localhost:8080/cache/stats | jq

# Non-streaming request
curl -X POST http://localhost:8080/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model": "test",
    "messages": [{"role": "user", "content": "Hello"}],
    "stream": false
  }'

# Streaming request
curl -X POST http://localhost:8080/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model": "test",
    "messages": [{"role": "user", "content": "Hello"}],
    "stream": true
  }' \
  --no-buffer
```

## 📊 Performance

NTONIX is designed for high-performance workloads:

- **Concurrent Connections**: 500-1000+ concurrent connections
- **Routing Latency**: <10ms p99 latency for request routing
- **Cache Hit Rate**: >30% with repeated prompts
- **Memory Efficiency**: Zero-copy streaming minimizes memory pressure
- **CPU Efficiency**: Async I/O with minimal thread overhead

## 🛠️ Tech Stack

| Component | Technology | Purpose |
|-----------|-----------|---------|
| **Language** | C++20 | Modern C++ features (std::jthread, std::shared_mutex, concepts) |
| **Networking** | Boost.Asio 1.83+ | Async I/O foundation |
| **HTTP** | Boost.Beast 1.83+ | HTTP/1.1 parsing and streaming |
| **Security** | OpenSSL 3.0+ | TLS/SSL termination |
| **JSON** | nlohmann/json 3.11+ | Configuration parsing |
| **Logging** | spdlog 1.12+ | Structured logging |
| **Hashing** | xxHash 0.8+ | Fast cache key generation |
| **Build** | CMake 3.20+ | Build system |
| **Containerization** | Docker | Deployment and testing |

## 📝 Development

### Project Structure

```
NTONIX/
├── CMakeLists.txt          # Build configuration
├── docker-compose.yml      # Docker Compose setup
├── Dockerfile              # Container image
├── config/
│   ├── ntonix.json         # Default configuration
│   └── docker-ntonix.json  # Docker configuration
├── src/
│   ├── main.cpp            # Entry point
│   ├── server/             # Server and connection handling
│   ├── proxy/              # Request forwarding and streaming
│   ├── balancer/           # Load balancing and health checks
│   ├── cache/              # LRU cache implementation
│   ├── config/             # Configuration management
│   └── util/               # Logging and metrics
├── tests/
│   └── integration/        # Integration tests (pytest)
├── mock/                   # Mock LLM backends (Python)
└── docs/
    └── adr/                # Architecture Decision Records
```

### Building

```bash
# Debug build
cmake -DCMAKE_BUILD_TYPE=Debug ..
cmake --build .

# Release build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --config Release
```

### Code Style

- Follow C++20 best practices
- Use RAII for resource management
- Prefer `std::shared_mutex` for reader-writer locks
- Use atomics for lock-free counters
- Document non-obvious design decisions in code comments

## 🐛 Troubleshooting

### Proxy won't start

- Check if port is already in use: `netstat -an | grep 8080`
- Verify configuration file syntax: `cat config/ntonix.json | jq`
- Check logs for error messages

### No healthy backends

- Verify backend health endpoints: `curl http://localhost:8001/health`
- Check health checker logs
- Ensure backends are accessible from proxy network

### Cache not working

- Verify cache is enabled in configuration
- Check cache statistics: `curl http://localhost:8080/cache/stats`
- Ensure requests are cacheable (non-streaming, 2xx responses)

### SSL errors

- Verify certificate and key files exist
- Check file permissions (readable by proxy process)
- Ensure certificate is valid and not expired

### Build errors (C++ compilation)

**"use of deleted function" with `std::atomic`**:
- `std::atomic` types are non-copyable and non-movable
- If you see errors about deleted copy/move constructors involving `std::atomic`, the containing struct is being copied/moved
- Solution: Use regular types with external synchronization (mutex) instead of `std::atomic` for struct members that need to be copyable

**"destructor is private" with `std::unique_ptr` singleton**:
- `std::unique_ptr` needs to call the destructor via `std::default_delete`
- If the destructor is private, `unique_ptr` cannot clean up the object
- Solution: Make the destructor public (keep constructor private for singleton pattern)


## 🙏 Acknowledgments

- **Boost.Asio/Beast** for excellent async networking libraries
- **OpenSSL** for robust TLS/SSL support
- **spdlog** for fast structured logging

