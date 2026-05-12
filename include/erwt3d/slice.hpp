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
    
    // Extents to read from file (before merging)
    std::vector<Extent> extents;
    
    // Output dimensions (for slice: 2D, for full restore: 3D)
    uint64_t out_dim0;
    uint64_t out_dim1;
    uint64_t out_dim2;
    
    // Copy instructions: for each leaf block
    struct CopyInstr {
        uint64_t src_offset;    // offset in read buffer (extent index)
        uint64_t base_dst_idx;  // precomputed 2D destination index
        uint32_t size_x, size_y, size_z;  // size to copy
        uint32_t src_off_x, src_off_y, src_off_z;  // offset within leaf block
    };
    std::vector<CopyInstr> copies;
    
    // --- Precomputed merged-extent mapping (filled by prepareSlicePlan) ---
    std::vector<Extent> merged_extents;           // merged extents
    std::vector<uint64_t> merged_buffer_offsets;  // byte offset of each merged extent in read buffer
    // For each copy instruction: index into merged_extents + byte offset within it
    std::vector<uint32_t> copy_merged_idx;
    std::vector<uint64_t> copy_merged_offset;
    bool prepared = false;
};

// Plan a slice read (raw extents, no merging)
SlicePlan planSlice(const ERWT3DHeader& header, const SliceRequest& request);

// Precompute merged extent mapping (call once after planSlice)
void prepareSlicePlan(SlicePlan& plan);

// Execute a slice read using a single contiguous read buffer
void executeSlice(const ERWT3DHeader& header, const SlicePlan& plan, 
                  const void* readBuffer, void* outputBuffer);

} // namespace erwt3d