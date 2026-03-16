#pragma once
#include <string>
#include <optional>
#include <unordered_map>
#include <list>
#include <shared_mutex>

// Thread-safe LRU cache for token → username lookups
// Used in the hot path (every authenticated request)
class TokenCache {
    size_t max_size_;
    std::list<std::pair<std::string, std::string>> lru_list_;  // token → username (MRU at front)
    std::unordered_map<std::string, decltype(lru_list_)::iterator> cache_map_;
    mutable std::shared_mutex mutex_;

public:
    explicit TokenCache(size_t max_size) : max_size_(max_size) {}

    // Get username for token (returns nullopt if not in cache)
    std::optional<std::string> get(const std::string& token) {
        std::unique_lock lock(mutex_);
        auto it = cache_map_.find(token);
        if(it == cache_map_.end()) {
            return std::nullopt;
        }
        // Move to front (most recently used)
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
        return it->second->second;
    }

    // Put token → username mapping in cache
    void put(const std::string& token, const std::string& username) {
        std::unique_lock lock(mutex_);
        auto it = cache_map_.find(token);
        if(it != cache_map_.end()) {
            // Already exists, update and move to front
            it->second->second = username;
            lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
            return;
        }

        // Add new entry at front
        lru_list_.emplace_front(token, username);
        cache_map_[token] = lru_list_.begin();

        // Evict LRU if over capacity
        if(cache_map_.size() > max_size_) {
            auto last = lru_list_.end();
            --last;
            cache_map_.erase(last->first);
            lru_list_.pop_back();
        }
    }

    // Invalidate a token (remove from cache)
    void invalidate(const std::string& token) {
        std::unique_lock lock(mutex_);
        auto it = cache_map_.find(token);
        if(it != cache_map_.end()) {
            lru_list_.erase(it->second);
            cache_map_.erase(it);
        }
    }

    // Clear all entries
    void clear() {
        std::unique_lock lock(mutex_);
        lru_list_.clear();
        cache_map_.clear();
    }

    // Get current size
    size_t size() const {
        std::shared_lock lock(mutex_);
        return cache_map_.size();
    }
};
