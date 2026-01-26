# NTONIX - High-Performance AI Inference Gateway
# Multi-stage Dockerfile for optimized build and runtime

# ============================================================================
# Stage 1: Build environment
# ============================================================================
FROM ubuntu:22.04 AS builder

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    git \
    ca-certificates \
    libboost-system-dev \
    libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Set working directory for build
WORKDIR /build

# Copy source files
COPY CMakeLists.txt .
COPY src/ src/

# Build the project
RUN cmake -B build \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-O3 -DNDEBUG" \
    && cmake --build build --parallel

# ============================================================================
# Stage 2: Runtime environment
# ============================================================================
FROM ubuntu:22.04 AS runtime

# Install runtime dependencies only
RUN apt-get update && apt-get install -y --no-install-recommends \
    libboost-system1.74.0 \
    libssl3 \
    ca-certificates \
    curl \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -r -s /bin/false ntonix

# Set working directory
WORKDIR /app

# Copy the built binary from builder stage
COPY --from=builder /build/build/ntonix /app/ntonix

# Copy default configuration
COPY config/ntonix.json /app/config/ntonix.json

# Create directory for SSL certificates (optional)
RUN mkdir -p /app/certs && chown -R ntonix:ntonix /app

# Switch to non-root user
USER ntonix

# Expose HTTP and HTTPS ports
EXPOSE 8080 8443

# Health check
HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=3 \
    CMD curl -f http://localhost:8080/health || exit 1

# Default command
ENTRYPOINT ["/app/ntonix"]
CMD ["--config", "/app/config/ntonix.json"]
