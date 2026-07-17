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
    eviction_count_ = 0;
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
