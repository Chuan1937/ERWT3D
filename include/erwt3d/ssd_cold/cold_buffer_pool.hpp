#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace erwt3d {
namespace ssd_cold {

struct AlignedBuffer {
    uint8_t* data = nullptr;
    uint64_t capacity = 0;
};

class ColdBufferPool {
public:
    static constexpr uint64_t kAlignment = 4096;

    ColdBufferPool() = default;
    ~ColdBufferPool();

    ColdBufferPool(const ColdBufferPool&) = delete;
    ColdBufferPool& operator=(const ColdBufferPool&) = delete;

    uint8_t* allocate(uint64_t size);
    void deallocate(uint8_t* ptr, uint64_t size);
    void releaseAll();

    uint64_t totalAllocatedBytes() const { return totalAllocated_; }
    uint64_t peakAllocatedBytes() const { return peakAllocated_; }

private:
    uint64_t totalAllocated_ = 0;
    uint64_t peakAllocated_ = 0;
};

} // namespace ssd_cold
} // namespace erwt3d
