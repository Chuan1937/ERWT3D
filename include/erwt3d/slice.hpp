#pragma once

#include "format.hpp"
#include "extent.hpp"
#include <cstdint>
#include <vector>

namespace erwt3d {

enum class SliceAxis {
    X, Y, Z
};

struct SliceRequest {
    SliceAxis axis;
    uint64_t index;
};

struct SlicePlan {
    SliceAxis axis;
    
    // Extents to read from file
    std::vector<Extent> extents;
    
    // Output dimensions (for slice: 2D, for full restore: 3D)
    uint64_t out_dim0;
    uint64_t out_dim1;
    uint64_t out_dim2;
    
    // Copy instructions: for each leaf block
    struct CopyInstr {
        uint64_t src_offset;    // offset in read buffer (extent index)
        uint64_t base_dst_idx;  // precomputed: (dst_z * out_dim0 + dst_y) * out_dim1 + dst_x
        uint32_t size_x, size_y, size_z;  // size to copy
        uint32_t src_off_x, src_off_y, src_off_z;  // offset within leaf block
    };
    std::vector<CopyInstr> copies;
};

// Plan a slice read
SlicePlan planSlice(const ERWT3DHeader& header, const SliceRequest& request);

// Execute a slice read
void executeSlice(const ERWT3DHeader& header, const SlicePlan& plan, 
                  const void* readBuffer, void* outputBuffer);

} // namespace erwt3d