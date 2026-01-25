/**
 * NTONIX - High-Performance AI Inference Gateway
 * Thread-Safe LRU Cache - Caches LLM responses keyed by prompt hash
 *
 * Features:
 * - Thread-safe with std::shared_mutex (concurrent reads, exclusive writes)
 * - LRU eviction when cache exceeds configured size
 * - Configurable TTL for cache entries
 * - Cache statistics for monitoring
 */

#ifndef NTONIX_CACHE_LRU_CACHE_HPP
#define NTONIX_CACHE_LRU_CACHE_HPP

#include "cache/cache_key.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace ntonix::cache {

/**
 * Cached response entry with metadata
 */
struct CacheEntry {
    std::string body;              // Response body
    std::string content_type;      // Content-Type header
    std::size_t size_bytes{0};     // Size of body in bytes

    std::chrono::steady_clock::time_point created_at;  // When entry was cached
    std::chrono::steady_clock::time_point last_access; // Last access time

    std::atomic<std::uint64_t> hit_count{0};  // Number of cache hits
};

/**
 * Cache statistics for monitoring
 */
struct CacheStats {
    std::uint64_t hits{0};          // Total cache hits
    std::uint64_t misses{0};        // Total cache misses
    std::uint64_t evictions{0};     // Total evictions
    std::uint64_t expired{0};       // Total expired entries removed

    std::size_t entries{0};         // Current number of entries
    std::size_t size_bytes{0};      // Current cache size in bytes
    std::size_t max_size_bytes{0};  // Maximum cache size in bytes

    double hit_rate() const {
        auto total = hits + misses;
        return total > 0 ? static_cast<double>(hits) / total : 0.0;
    }
};

/**
 * LRU Cache configuration
 */
struct LruCacheConfig {
    std::size_t max_size_bytes{512 * 1024 * 1024};  // 512 MB default
    std::chrono::seconds ttl{3600};                  // 1 hour TTL default
    bool enabled{true};                              // Cache enabled flag
};

/**
 * Thread-safe LRU cache for LLM responses
 *
 * Uses std::shared_mutex for concurrent read access with minimal lock contention.
 * Write operations (put, eviction) acquire exclusive locks.
 *
 * Implementation:
 * - Hash map for O(1) lookup by cache key
 * - Doubly-linked list for LRU ordering
 * - Size-based eviction when max_size_bytes exceeded
 * - TTL-based expiration checked on access
 */
class LruCache {
public:
    explicit LruCache(const LruCacheConfig& config);
    ~LruCache() = default;

    // Non-copyable, non-movable
    LruCache(const LruCache&) = delete;
    LruCache& operator=(const LruCache&) = delete;
    LruCache(LruCache&&) = delete;
    LruCache& operator=(LruCache&&) = delete;

    /**
     * Get a cached response by key
     *
     * @param key Cache key
     * @return Cached entry if found and not expired, nullopt otherwise
     */
    std::optional<CacheEntry> get(const CacheKey& key);

    /**
     * Store a response in the cache
     *
     * @param key Cache key
     * @param body Response body
     * @param content_type Content-Type header
     */
    void put(const CacheKey& key, std::string body, std::string content_type);

    /**
     * Remove an entry from the cache
     *
     * @param key Cache key
     * @return true if entry was removed, false if not found
     */
    bool remove(const CacheKey& key);

    /**
     * Clear all entries from the cache
     */
    void clear();

    /**
     * Get cache statistics (thread-safe)
     */
    CacheStats get_stats() const;

    /**
     * Check if cache is enabled
     */
    bool is_enabled() const { return config_.enabled; }

    /**
     * Update configuration (thread-safe)
     * Note: Only max_size_bytes and ttl can be updated at runtime
     */
    void update_config(std::size_t max_size_bytes, std::chrono::seconds ttl);

private:
    // Internal node for LRU list
    struct Node {
        CacheKey key;
        CacheEntry entry;
    };

    using LruList = std::list<Node>;
    using CacheMap = std::unordered_map<CacheKey, typename LruList::iterator, CacheKeyHash>;

    /**
     * Move node to front of LRU list (most recently used)
     * Must be called with exclusive lock held
     */
    void touch_node(typename LruList::iterator it);

    /**
     * Evict entries until cache size is below max
     * Must be called with exclusive lock held
     */
    void evict_if_needed();

    /**
     * Check if an entry has expired
     */
    bool is_expired(const CacheEntry& entry) const;

    mutable std::shared_mutex mutex_;
    LruCacheConfig config_;

    LruList lru_list_;  // Front = most recently used, back = least recently used
    CacheMap cache_map_;

    // Statistics (atomic for lock-free reads)
    std::atomic<std::uint64_t> hits_{0};
    std::atomic<std::uint64_t> misses_{0};
    std::atomic<std::uint64_t> evictions_{0};
    std::atomic<std::uint64_t> expired_{0};
    std::atomic<std::size_t> current_size_bytes_{0};
};

} // namespace ntonix::cache

#endif // NTONIX_CACHE_LRU_CACHE_HPP
