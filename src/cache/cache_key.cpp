/**
 * NTONIX - High-Performance AI Inference Gateway
 * Cache Key Implementation - XXHash-based prompt hashing
 */

#include "cache/cache_key.hpp"

#include <xxhash.h>

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace ntonix::cache {

std::string CacheKey::to_string() const {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << hash;
    return oss.str();
}

CacheKey generate_cache_key(std::string_view body) {
    // Use XXH64 for fast, high-quality hashing
    CacheKey key;
    key.hash = XXH64(body.data(), body.size(), 0);
    return key;
}

CacheKey generate_cache_key(std::string_view method, std::string_view target, std::string_view body) {
    // Create a composite hash from method, target, and body
    // Use XXH3 state for incremental hashing
    XXH64_state_t* state = XXH64_createState();
    XXH64_reset(state, 0);

    XXH64_update(state, method.data(), method.size());
    XXH64_update(state, ":", 1);
    XXH64_update(state, target.data(), target.size());
    XXH64_update(state, ":", 1);
    XXH64_update(state, body.data(), body.size());

    CacheKey key;
    key.hash = XXH64_digest(state);
    XXH64_freeState(state);

    return key;
}

bool should_bypass_cache(std::string_view cache_control) {
    if (cache_control.empty()) {
        return false;
    }

    // Convert to lowercase for case-insensitive comparison
    std::string lower(cache_control);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Check for cache bypass directives
    return lower.find("no-cache") != std::string::npos ||
           lower.find("no-store") != std::string::npos;
}

} // namespace ntonix::cache
