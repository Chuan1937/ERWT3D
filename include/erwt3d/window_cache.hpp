#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace erwt3d {

struct WindowCacheKey {
    uint64_t file_identity = 0;
    uint64_t offset = 0;
    uint64_t size = 0;

    bool operator==(const WindowCacheKey& other) const {
        return file_identity == other.file_identity &&
               offset == other.offset &&
               size == other.size;
    }
};

struct WindowCacheKeyHash {
    std::size_t operator()(const WindowCacheKey& key) const {
        std::size_t h = std::hash<uint64_t>()(key.file_identity);
        h ^= std::hash<uint64_t>()(key.offset) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<uint64_t>()(key.size) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

class BoundedWindowCache {
public:
    explicit BoundedWindowCache(uint64_t capacity_bytes = 0);

    bool get(
        const WindowCacheKey& key,
        std::shared_ptr<const std::vector<uint8_t>>& data
    );

    bool put(
        const WindowCacheKey& key,
        std::vector<uint8_t>&& data
    );

    void setCapacityBytes(uint64_t capacity_bytes);
    void clear();

    uint64_t capacityBytes() const;
    uint64_t residentBytes() const;
    uint64_t hitCount() const;
    uint64_t missCount() const;
    uint64_t evictionCount() const;

private:
    struct Entry {
        WindowCacheKey key;
        std::shared_ptr<const std::vector<uint8_t>> data;
        uint64_t bytes = 0;
    };

    using LruList = std::list<Entry>;
    using Index = std::unordered_map<
        WindowCacheKey,
        LruList::iterator,
        WindowCacheKeyHash
    >;

    void evictToCapacityLocked();

    mutable std::mutex mutex_;
    uint64_t capacity_bytes_ = 0;
    uint64_t resident_bytes_ = 0;
    uint64_t hit_count_ = 0;
    uint64_t miss_count_ = 0;
    uint64_t eviction_count_ = 0;
    LruList lru_;
    Index index_;
};

} // namespace erwt3d
