#pragma once
#include "hnswlib.h"
#include "../utils/settings.hpp"
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <cstring>
#include <array>
#include <limits>
#include <cstdlib>
#include <string>

namespace hnswlib {

class VectorCache {
public:
    inline static size_t VECTOR_CACHE_PERCENTAGE = settings::VECTOR_CACHE_PERCENTAGE;

    inline static size_t VECTOR_CACHE_MIN_BITS = settings::VECTOR_CACHE_MIN_BITS;
    static constexpr uint8_t MAX_COUNTER = 2; // Sticky replacement policy
    // Helper to calculate required cache bits based on element count and percentage
    static size_t calculateCacheBits(size_t element_count, size_t cache_percent = VECTOR_CACHE_PERCENTAGE) {
        if (element_count == 0 || cache_percent == 0) return 0;
        
        size_t target_elements = (element_count * cache_percent) / 100;
        
        // Calculate bits needed: 2^bits >= target_elements
        size_t bits = 0;
        while ((1ULL << bits) < target_elements) {
            bits++;
        }
        
        // Enforce minimum bits
        if (bits < VECTOR_CACHE_MIN_BITS) {
            bits = VECTOR_CACHE_MIN_BITS;
        }

        return bits;
    }

private:
    size_t cacheBits_ = 0;
    size_t cacheSize_ = 0;
    size_t cacheMask_ = 0;
    size_t vectorCacheDataSize_ = 0;
    size_t data_size_ = 0;
    uint8_t* vectorCache_ = nullptr;
    
    static constexpr size_t CACHE_STRIPE_BITS = 8; // 256 stripes
    static constexpr size_t CACHE_STRIPE_COUNT = 1 << CACHE_STRIPE_BITS;
    static constexpr size_t CACHE_STRIPE_MASK = CACHE_STRIPE_COUNT - 1;
    mutable std::array<std::shared_mutex, CACHE_STRIPE_COUNT> vectorCacheStripeMutexes_;
    
    static constexpr idInt INVALID_ID = static_cast<idInt>(-1);

    std::shared_mutex& getCacheStripeMutex(size_t cache_index) const {
        size_t stripe_id = cache_index & CACHE_STRIPE_MASK;
        return vectorCacheStripeMutexes_[stripe_id];
    }

public:
    VectorCache() = default;
    
    // Constructor with initialization
    VectorCache(size_t data_size, size_t cache_bits) {
        init(data_size, cache_bits);
    }
    
    ~VectorCache() {
        if (vectorCache_) {
            delete[] vectorCache_;
            vectorCache_ = nullptr;
        }
    }
    
    void init(size_t data_size, size_t cache_bits) {
        if (vectorCache_) {
            delete[] vectorCache_;
            vectorCache_ = nullptr;
        }

        if (cache_bits == 0) {
            cacheBits_ = 0;
            cacheSize_ = 0;
            cacheMask_ = 0;
            data_size_ = 0;
            vectorCacheDataSize_ = 0;
            return;
        }

        data_size_ = data_size;
        cacheBits_ = cache_bits;
        cacheSize_ = 1 << cacheBits_;
        cacheMask_ = cacheSize_ - 1;
        // Layout: [idInt] [uint8_t counter] [data...]
        vectorCacheDataSize_ = data_size_ + sizeof(idInt) + sizeof(uint8_t);
        
        vectorCache_ = new uint8_t[cacheSize_ * vectorCacheDataSize_];
        
        // Initialize all entries to INVALID_ID
        for (size_t i = 0; i < cacheSize_; i++) {
            uint8_t* entry = vectorCache_ + i * vectorCacheDataSize_;
            idInt* id_ptr = reinterpret_cast<idInt*>(entry);
            *id_ptr = INVALID_ID;
            // Also zero out counter/data for cleanliness
            *(entry + sizeof(idInt)) = 0;
        }
    }
    
    bool get(idInt internal_id, uint8_t* buffer) const {
        if (!vectorCache_) return false;
        
        size_t index = internal_id & cacheMask_;
        uint8_t* entry = vectorCache_ + index * vectorCacheDataSize_;
        
        std::shared_lock<std::shared_mutex> lock(getCacheStripeMutex(index));
        
        idInt* stored_id = reinterpret_cast<idInt*>(entry);
        if (*stored_id == internal_id) {
            // Hit! Reset counter to MAX_COUNTER (stickiness)
            // Optimization: Only write if currently different to avoid cache line invalidation (False Sharing)
            uint8_t* counter_ptr = entry + sizeof(idInt);
            auto atomic_counter = reinterpret_cast<std::atomic<uint8_t>*>(counter_ptr);
            
            if (atomic_counter->load(std::memory_order_relaxed) < MAX_COUNTER) {
                 atomic_counter->store(MAX_COUNTER, std::memory_order_relaxed);
            }

            memcpy(buffer, entry + sizeof(idInt) + sizeof(uint8_t), data_size_);
            return true;
        }
        return false;
    }
    
    void insert(idInt internal_id, const uint8_t* data) {
        if (!vectorCache_) return;
        
        size_t index = internal_id & cacheMask_;
        uint8_t* entry = vectorCache_ + index * vectorCacheDataSize_;
        
        std::unique_lock<std::shared_mutex> lock(getCacheStripeMutex(index));
        
        idInt* stored_id = reinterpret_cast<idInt*>(entry);
        // Use atomic consistently to avoid UB, though we are under unique_lock
        auto atomic_counter = reinterpret_cast<std::atomic<uint8_t>*>(entry + sizeof(idInt));
        uint8_t* data_ptr = entry + sizeof(idInt) + sizeof(uint8_t);

        if (*stored_id == internal_id) {
            // Update existing
            atomic_counter->store(MAX_COUNTER, std::memory_order_relaxed);
            memcpy(data_ptr, data, data_size_);
            return;
        }
        
        if (*stored_id == INVALID_ID) {
            // Empty slot
            *stored_id = internal_id;
            atomic_counter->store(MAX_COUNTER, std::memory_order_relaxed);
            memcpy(data_ptr, data, data_size_);
            return;
        }

        // Collision with different vector
        uint8_t c = atomic_counter->load(std::memory_order_relaxed);
        if (c > 0) {
            c--;
            atomic_counter->store(c, std::memory_order_relaxed);
        }
        
        if (c == 0) {
            // Replace
            *stored_id = internal_id;
            atomic_counter->store(MAX_COUNTER, std::memory_order_relaxed);
            memcpy(data_ptr, data, data_size_);
        }
        // Else: reject new vector, keep old one (thrashing protection)
    }
    
    size_t getCacheBits() const { return cacheBits_; }
    size_t getCacheSize() const { return cacheSize_; }
    void setCacheBits(size_t bits) { cacheBits_ = bits; }
    
    size_t getMemoryUsage() const {
        if (!vectorCache_) return 0;
        return cacheSize_ * vectorCacheDataSize_;
    }
};

} // namespace hnswlib
