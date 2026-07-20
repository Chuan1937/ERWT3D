#include "erwt3d/window_cache.hpp"

#include <iterator>
#include <utility>

namespace erwt3d {

BoundedWindowCache::BoundedWindowCache(uint64_t capacity_bytes)
    : capacity_bytes_(capacity_bytes) {}

bool BoundedWindowCache::get(
    const WindowCacheKey& key,
    std::shared_ptr<const std::vector<uint8_t>>& data
) {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = index_.find(key);
    if (it == index_.end()) {
        ++miss_count_;
        data.reset();
        return false;
    }

    lru_.splice(lru_.begin(), lru_, it->second);
    data = it->second->data;
    ++hit_count_;
    return true;
}

bool BoundedWindowCache::getContaining(
    uint64_t file_identity,
    uint64_t offset,
    uint64_t size,
    std::shared_ptr<const std::vector<uint8_t>>& data,
    uint64_t* cached_offset
) {
    if (size == 0) {
        data.reset();
        return false;
    }
    const uint64_t request_end = offset + size;

    std::lock_guard<std::mutex> lock(mutex_);

    for (auto it = lru_.begin(); it != lru_.end(); ++it) {
        if (it->key.file_identity != file_identity) continue;
        const uint64_t cached_end = it->key.offset + it->key.size;
        if (it->key.offset <= offset && cached_end >= request_end) {
            lru_.splice(lru_.begin(), lru_, it);
            data = it->data;
            if (cached_offset) *cached_offset = it->key.offset;
            ++contained_hit_count_;
            saved_read_bytes_ += size;
            return true;
        }
    }

    data.reset();
    return false;
}

bool BoundedWindowCache::put(
    const WindowCacheKey& key,
    std::vector<uint8_t>&& data
) {
    auto shared = std::make_shared<const std::vector<uint8_t>>(std::move(data));
    return putShared(key, std::move(shared));
}

bool BoundedWindowCache::putShared(
    const WindowCacheKey& key,
    std::shared_ptr<const std::vector<uint8_t>> data
) {
    if (!data) return false;
    const uint64_t bytes = static_cast<uint64_t>(data->size());

    std::lock_guard<std::mutex> lock(mutex_);

    const auto existing = index_.find(key);
    if (existing != index_.end()) {
        resident_bytes_ -= existing->second->bytes;
        lru_.erase(existing->second);
        index_.erase(existing);
    }

    if (capacity_bytes_ == 0 || bytes == 0 || bytes > capacity_bytes_) {
        return false;
    }

    lru_.push_front(Entry{key, std::move(data), bytes});
    index_[key] = lru_.begin();
    resident_bytes_ += bytes;

    evictToCapacityLocked();
    return index_.find(key) != index_.end();
}

void BoundedWindowCache::setCapacityBytes(uint64_t capacity_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    capacity_bytes_ = capacity_bytes;
    evictToCapacityLocked();
}

void BoundedWindowCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    lru_.clear();
    index_.clear();
    resident_bytes_ = 0;
    hit_count_ = 0;
    miss_count_ = 0;
    contained_hit_count_ = 0;
    eviction_count_ = 0;
    saved_read_bytes_ = 0;
}

uint64_t BoundedWindowCache::capacityBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return capacity_bytes_;
}

uint64_t BoundedWindowCache::residentBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return resident_bytes_;
}

uint64_t BoundedWindowCache::hitCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hit_count_;
}

uint64_t BoundedWindowCache::missCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return miss_count_;
}

uint64_t BoundedWindowCache::containedHitCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return contained_hit_count_;
}

uint64_t BoundedWindowCache::savedReadBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return saved_read_bytes_;
}

uint64_t BoundedWindowCache::evictionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return eviction_count_;
}

void BoundedWindowCache::evictToCapacityLocked() {
    while (resident_bytes_ > capacity_bytes_ && !lru_.empty()) {
        auto last = std::prev(lru_.end());
        resident_bytes_ -= last->bytes;
        index_.erase(last->key);
        lru_.erase(last);
        ++eviction_count_;
    }
}

} // namespace erwt3d
