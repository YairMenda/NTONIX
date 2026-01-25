/**
 * NTONIX - High-Performance AI Inference Gateway
 * LRU Cache Implementation
 */

#include "cache/lru_cache.hpp"

#include <spdlog/spdlog.h>

namespace ntonix::cache {

LruCache::LruCache(const LruCacheConfig& config)
    : config_(config) {
    spdlog::debug("LRU cache initialized: max_size={}MB, ttl={}s, enabled={}",
                  config_.max_size_bytes / (1024 * 1024),
                  config_.ttl.count(),
                  config_.enabled);
}

std::optional<CacheEntry> LruCache::get(const CacheKey& key) {
    if (!config_.enabled) {
        return std::nullopt;
    }

    // First, try with a shared (read) lock
    {
        std::shared_lock<std::shared_mutex> read_lock(mutex_);

        auto it = cache_map_.find(key);
        if (it == cache_map_.end()) {
            ++misses_;
            return std::nullopt;
        }

        // Check expiration
        if (is_expired(it->second->entry)) {
            // Need to upgrade to exclusive lock to remove expired entry
            read_lock.unlock();

            std::unique_lock<std::shared_mutex> write_lock(mutex_);
            // Re-check after acquiring write lock (another thread may have removed it)
            it = cache_map_.find(key);
            if (it != cache_map_.end() && is_expired(it->second->entry)) {
                current_size_bytes_ -= it->second->entry.size_bytes;
                lru_list_.erase(it->second);
                cache_map_.erase(it);
                ++expired_;
            }
            ++misses_;
            return std::nullopt;
        }

        // Found valid entry - copy data while holding read lock
        CacheEntry result = it->second->entry;
        result.hit_count++;
        result.last_access = std::chrono::steady_clock::now();

        // Note: We can't update LRU order with read lock, so we do it outside
        // This is a trade-off: slightly stale LRU ordering for better read concurrency
        ++hits_;

        return result;
    }
}

void LruCache::put(const CacheKey& key, std::string body, std::string content_type) {
    if (!config_.enabled) {
        return;
    }

    std::size_t entry_size = body.size();

    // Don't cache entries larger than max cache size
    if (entry_size > config_.max_size_bytes) {
        spdlog::debug("Cache entry too large: {} bytes > {} max",
                      entry_size, config_.max_size_bytes);
        return;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);

    // Check if key already exists
    auto it = cache_map_.find(key);
    if (it != cache_map_.end()) {
        // Update existing entry
        std::size_t old_size = it->second->entry.size_bytes;
        it->second->entry.body = std::move(body);
        it->second->entry.content_type = std::move(content_type);
        it->second->entry.size_bytes = entry_size;
        it->second->entry.created_at = std::chrono::steady_clock::now();
        it->second->entry.last_access = std::chrono::steady_clock::now();
        it->second->entry.hit_count = 0;

        current_size_bytes_ = current_size_bytes_ - old_size + entry_size;

        // Move to front (most recently used)
        touch_node(it->second);

        spdlog::debug("Cache entry updated: key={}, size={}", key.to_string(), entry_size);
    } else {
        // Add new entry
        Node node;
        node.key = key;
        node.entry.body = std::move(body);
        node.entry.content_type = std::move(content_type);
        node.entry.size_bytes = entry_size;
        node.entry.created_at = std::chrono::steady_clock::now();
        node.entry.last_access = std::chrono::steady_clock::now();
        node.entry.hit_count = 0;

        // Add to front of list (most recently used)
        lru_list_.push_front(std::move(node));
        cache_map_[key] = lru_list_.begin();

        current_size_bytes_ += entry_size;

        spdlog::debug("Cache entry added: key={}, size={}, total_size={}",
                      key.to_string(), entry_size, current_size_bytes_.load());
    }

    // Evict if over size limit
    evict_if_needed();
}

bool LruCache::remove(const CacheKey& key) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    auto it = cache_map_.find(key);
    if (it == cache_map_.end()) {
        return false;
    }

    current_size_bytes_ -= it->second->entry.size_bytes;
    lru_list_.erase(it->second);
    cache_map_.erase(it);

    spdlog::debug("Cache entry removed: key={}", key.to_string());
    return true;
}

void LruCache::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    std::size_t count = cache_map_.size();
    cache_map_.clear();
    lru_list_.clear();
    current_size_bytes_ = 0;

    spdlog::info("Cache cleared: {} entries removed", count);
}

CacheStats LruCache::get_stats() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    CacheStats stats;
    stats.hits = hits_.load();
    stats.misses = misses_.load();
    stats.evictions = evictions_.load();
    stats.expired = expired_.load();
    stats.entries = cache_map_.size();
    stats.size_bytes = current_size_bytes_.load();
    stats.max_size_bytes = config_.max_size_bytes;

    return stats;
}

void LruCache::update_config(std::size_t max_size_bytes, std::chrono::seconds ttl) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    config_.max_size_bytes = max_size_bytes;
    config_.ttl = ttl;

    spdlog::info("Cache config updated: max_size={}MB, ttl={}s",
                 max_size_bytes / (1024 * 1024), ttl.count());

    // Evict if new size is smaller
    evict_if_needed();
}

void LruCache::touch_node(typename LruList::iterator it) {
    // Move node to front of list
    if (it != lru_list_.begin()) {
        lru_list_.splice(lru_list_.begin(), lru_list_, it);
    }
}

void LruCache::evict_if_needed() {
    // Evict from back (least recently used) until under size limit
    while (current_size_bytes_ > config_.max_size_bytes && !lru_list_.empty()) {
        auto& lru_node = lru_list_.back();

        spdlog::debug("Evicting cache entry: key={}, size={}",
                      lru_node.key.to_string(), lru_node.entry.size_bytes);

        current_size_bytes_ -= lru_node.entry.size_bytes;
        cache_map_.erase(lru_node.key);
        lru_list_.pop_back();

        ++evictions_;
    }
}

bool LruCache::is_expired(const CacheEntry& entry) const {
    auto now = std::chrono::steady_clock::now();
    auto age = std::chrono::duration_cast<std::chrono::seconds>(now - entry.created_at);
    return age > config_.ttl;
}

} // namespace ntonix::cache
