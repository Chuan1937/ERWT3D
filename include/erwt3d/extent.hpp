#pragma once

#include <cstdint>
#include <vector>

namespace erwt3d {

struct Extent {
    uint64_t offset;
    uint64_t size;
    
    Extent(uint64_t o, uint64_t s) : offset(o), size(s) {}
    
    uint64_t end() const { return offset + size; }
};

// Merge adjacent extents to reduce syscall overhead
std::vector<Extent> mergeExtents(const std::vector<Extent>& extents);

} // namespace erwt3d