#include "erwt3d/cache.hpp"
#include <cstring>

namespace erwt3d {

LeafCache::LeafCache(size_t maxBytes) 
    : maxSize_(maxBytes), currentSize_(0) {
}

LeafCache::~LeafCache() {
}

bool LeafCache::get(uint64_t key, void* data, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = index_.find(key);
    if (it == index_.end()) {
        return false;
    }
    
    // Move to front of LRU list
    lruList_.splice(lruList_.begin(), lruList_, it->second);
    
    // Copy data
    std::memcpy(data, it->second->data.data(), size);
    return true;
}

void LeafCache::put(uint64_t key, const void* data, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Check if already exists
    auto it = index_.find(key);
    if (it != index_.end()) {
        // Move to front
        lruList_.splice(lruList_.begin(), lruList_, it->second);
        return;
    }
    
    // Evict if necessary
    while (currentSize_ + size > maxSize_ && !lruList_.empty()) {
        auto& last = lruList_.back();
        currentSize_ -= last.data.size();
        index_.erase(last.key);
        lruList_.pop_back();
    }
    
    // Add new entry
    CacheEntry entry;
    entry.key = key;
    entry.data.resize(size);
    std::memcpy(entry.data.data(), data, size);
    
    lruList_.push_front(std::move(entry));
    index_[key] = lruList_.begin();
    currentSize_ += size;
}

void LeafCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    lruList_.clear();
    index_.clear();
    currentSize_ = 0;
}

} // namespace erwt3d