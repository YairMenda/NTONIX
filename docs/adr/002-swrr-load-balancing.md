# ADR-002: Smooth Weighted Round-Robin Load Balancing

## Status
Accepted

## Context
NTONIX needs to distribute requests across multiple LLM backend nodes. The distribution algorithm must:
- Support weighted distribution for heterogeneous hardware (e.g., different GPU capabilities)
- Be thread-safe for concurrent request routing
- Skip unhealthy backends automatically
- Provide predictable, even distribution

## Decision
We chose **Smooth Weighted Round-Robin (SWRR)** algorithm, as implemented by NGINX.

### Alternatives Considered

1. **Simple Round-Robin**
   - Pros: Simple to implement
   - Cons: No weight support, can't handle heterogeneous backends

2. **Random Selection**
   - Pros: Simple, no shared state
   - Cons: Unpredictable distribution, may not respect weights accurately

3. **Least Connections**
   - Pros: Naturally balances slow backends
   - Cons: Requires tracking active connections per backend, more complex state

4. **Consistent Hashing**
   - Pros: Good for session affinity
   - Cons: Overkill for LLM requests, uneven distribution with few backends

### Why SWRR

The SWRR algorithm provides smooth distribution that spreads requests evenly over time, rather than clustering weighted backends together.

**Example with weights [5, 1, 1]:**
- Simple weighted RR: A, A, A, A, A, B, C (clustered)
- SWRR: A, A, B, A, A, C, A (spread evenly)

This smoother distribution prevents burst traffic to any single backend.

## Algorithm

```
For each backend i:
  current_weight[i] += effective_weight[i]

Select backend with max current_weight
Subtract total_weight from selected backend's current_weight
```

## Implementation

```cpp
BackendConfig* LoadBalancer::select_backend() {
    std::lock_guard<std::mutex> lock(mutex_);

    BackendConfig* best = nullptr;
    int max_weight = 0;
    int total_weight = 0;

    for (auto& backend : backends_) {
        if (!backend.healthy) continue;

        backend.current_weight += backend.effective_weight;
        total_weight += backend.effective_weight;

        if (backend.current_weight > max_weight) {
            max_weight = backend.current_weight;
            best = &backend;
        }
    }

    if (best) {
        best->current_weight -= total_weight;
    }

    return best;
}
```

## Consequences

### Positive
- Even distribution over time regardless of weights
- Thread-safe with minimal lock contention
- Deterministic behavior aids debugging
- Well-understood algorithm with proven track record

### Negative
- Requires mutex for thread safety (but lock time is minimal)
- State must be maintained across requests

### Health Integration
The load balancer integrates with the health checker:
- `effective_weight` is reduced when backends fail health checks
- Backends with `healthy = false` are skipped entirely
- When health recovers, `effective_weight` gradually increases

## References
- [NGINX Upstream Round-Robin](https://www.nginx.com/blog/nginx-power-of-two-choices-load-balancing-algorithm/)
- [Smooth Weighted Round-Robin Balancing](https://github.com/phusion/nginx/commit/27e94984486058d73157038f7950a0a36ecc6e35)
