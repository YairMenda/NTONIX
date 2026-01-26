# ADR-003: Thread-Safe LRU Cache with shared_mutex

## Status
Accepted

## Context
NTONIX caches LLM responses to avoid redundant inference for identical prompts. The cache must:
- Support high-concurrency read access (cache hits are common)
- Handle occasional writes for new entries
- Implement LRU eviction when size limits are reached
- Support TTL-based expiration
- Be bounded by configurable memory limits

## Decision
We implemented a custom **Thread-Safe LRU Cache** using `std::shared_mutex` for reader-writer locking with `std::list` and `std::unordered_map` for O(1) operations.

### Alternatives Considered

1. **Lock-free data structures**
   - Pros: Maximum concurrency
   - Cons: Complex implementation, LRU ordering difficult without locks

2. **Per-shard locking (like ConcurrentHashMap)**
   - Pros: Reduced contention
   - Cons: LRU ordering across shards is complex

3. **External cache (Redis, Memcached)**
   - Pros: Mature, distributed
   - Cons: Network latency, external dependency, overkill for local cache

4. **Single mutex**
   - Pros: Simple
   - Cons: Serializes all access, bottleneck under high read load

### Why shared_mutex

LLM inference workloads have a specific access pattern:
- **Read-heavy**: Most requests hit the cache (>40% target)
- **Occasional writes**: New prompts require cache insertion
- **Infrequent eviction**: LRU eviction happens periodically

`std::shared_mutex` allows:
- Multiple concurrent readers (shared lock)
- Exclusive access for writers (unique lock)

## Implementation

```cpp
template <typename Key, typename Value>
class LruCache {
    struct Entry {
        Key key;
        Value value;
        std::chrono::steady_clock::time_point expiry;
        size_t size_bytes;
    };

    std::list<Entry> lru_list_;                    // Most recent at front
    std::unordered_map<Key, typename std::list<Entry>::iterator> lookup_;
    mutable std::shared_mutex mutex_;

    size_t max_size_bytes_;
    size_t current_size_bytes_ = 0;
};
```

### Cache Key Design
Cache keys are computed using xxHash on the request body, including:
- Prompt content
- Model name
- Temperature and other sampling parameters

This ensures semantically identical requests hit the cache while different parameters generate separate entries.

## Operations

### Get (Read)
```cpp
std::optional<Value> get(const Key& key) {
    std::shared_lock lock(mutex_);  // Shared lock for reads
    auto it = lookup_.find(key);
    if (it == lookup_.end()) return std::nullopt;
    if (is_expired(it->second)) return std::nullopt;
    // Move to front (requires upgrade to unique lock)
    return it->second->value;
}
```

### Put (Write)
```cpp
void put(const Key& key, Value value, size_t size_bytes) {
    std::unique_lock lock(mutex_);  // Exclusive lock for writes
    evict_if_needed(size_bytes);
    // Insert at front of LRU list
    lru_list_.push_front({key, std::move(value), compute_expiry(), size_bytes});
    lookup_[key] = lru_list_.begin();
    current_size_bytes_ += size_bytes;
}
```

## Consequences

### Positive
- High read throughput with shared locks
- O(1) lookup and LRU operations
- Bounded memory usage
- TTL prevents stale data

### Negative
- Readers must upgrade to unique lock to update LRU position
- Single cache instance (could shard for higher concurrency)

### Trade-offs
- We chose simplicity over maximum concurrency
- LRU position update on read requires lock upgrade (acceptable for our load)

## Metrics
The cache exposes statistics via the `/metrics` endpoint:
- `cache_hits`: Total cache hits
- `cache_misses`: Total cache misses
- `cache_hit_rate`: hits / (hits + misses)
- `cache_size_bytes`: Current memory usage

## Cache Bypass
Clients can bypass the cache using the `Cache-Control: no-cache` header, useful for:
- Forcing fresh inference
- Debugging cache behavior
- Non-deterministic requests
