/**
 * NTONIX - High-Performance AI Inference Gateway
 * Cache Key - XXHash-based prompt hashing for cache keys
 *
 * Creates unique cache keys from request content including:
 * - Request body (prompt/messages)
 * - Model name
 * - Temperature and other generation parameters
 */

#ifndef NTONIX_CACHE_CACHE_KEY_HPP
#define NTONIX_CACHE_CACHE_KEY_HPP

#include <cstdint>
#include <string>
#include <string_view>

namespace ntonix::cache {

/**
 * Cache key - 64-bit hash of request content
 */
struct CacheKey {
    std::uint64_t hash{0};

    bool operator==(const CacheKey& other) const {
        return hash == other.hash;
    }

    bool operator<(const CacheKey& other) const {
        return hash < other.hash;
    }

    /**
     * Convert to hex string for logging/debugging
     */
    std::string to_string() const;
};

/**
 * Hash functor for use with std::unordered_map
 */
struct CacheKeyHash {
    std::size_t operator()(const CacheKey& key) const noexcept {
        return static_cast<std::size_t>(key.hash);
    }
};

/**
 * Generate a cache key from request body
 *
 * For LLM requests, the body typically contains:
 * - model: The model name
 * - messages: The conversation history
 * - temperature: Sampling temperature
 * - max_tokens: Maximum tokens to generate
 *
 * All of these affect the response, so they're included in the hash.
 *
 * @param body The request body (JSON string)
 * @return Cache key
 */
CacheKey generate_cache_key(std::string_view body);

/**
 * Generate a cache key from multiple components
 *
 * @param method HTTP method
 * @param target Request target (URI)
 * @param body Request body
 * @return Cache key
 */
CacheKey generate_cache_key(std::string_view method, std::string_view target, std::string_view body);

/**
 * Check if a request should bypass cache based on headers
 *
 * @param cache_control Value of Cache-Control header
 * @return true if cache should be bypassed
 */
bool should_bypass_cache(std::string_view cache_control);

} // namespace ntonix::cache

#endif // NTONIX_CACHE_CACHE_KEY_HPP
