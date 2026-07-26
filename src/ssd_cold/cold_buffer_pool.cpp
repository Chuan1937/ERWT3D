#include "erwt3d/ssd_cold/cold_buffer_pool.hpp"

#include <cstdlib>
#include <cstring>

namespace erwt3d {
namespace ssd_cold {

ColdBufferPool::~ColdBufferPool() {
    releaseAll();
}

uint8_t* ColdBufferPool::allocate(uint64_t size) {
    const uint64_t aligned = (size + kAlignment - 1) & ~(kAlignment - 1);
    uint8_t* ptr = static_cast<uint8_t*>(std::aligned_alloc(kAlignment, aligned));
    if (ptr) {
        totalAllocated_ += aligned;
        if (totalAllocated_ > peakAllocated_) peakAllocated_ = totalAllocated_;
    }
    return ptr;
}

void ColdBufferPool::deallocate(uint8_t* ptr, uint64_t size) {
    if (ptr) {
        const uint64_t aligned = (size + kAlignment - 1) & ~(kAlignment - 1);
        if (aligned <= totalAllocated_) totalAllocated_ -= aligned;
        std::free(ptr);
    }
}

void ColdBufferPool::releaseAll() {
    totalAllocated_ = 0;
}

} // namespace ssd_cold
} // namespace erwt3d
