#pragma once

#include <cstdint>
#include <unordered_map>
#include <list>
#include <vector>
#include <mutex>

namespace erwt3d {

class LeafCache {
public:
    LeafCache(size_t maxBytes);
    ~LeafCache();
    
    // Try to get a cached leaf block
    bool get(uint64_t key, void* data, size_t size);
    
    // Store a leaf block in cache
    void put(uint64_t key, const void* data, size_t size);
    
    // Clear the cache
    void clear();
    
    // Get current cache size in bytes
    size_t currentSize() const { return currentSize_; }
    
    // Get max cache size in bytes
    size_t maxSize() const { return maxSize_; }

private:
    struct CacheEntry {
        uint64_t key;
        std::vector<uint8_t> data;
    };
    
    size_t maxSize_;
    size_t currentSize_;
    std::list<CacheEntry> lruList_;
    std::unordered_map<uint64_t, std::list<CacheEntry>::iterator> index_;
    mutable std::mutex mutex_;
};

} // namespace erwt3d