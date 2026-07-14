#pragma once

#include "format.hpp"
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace erwt3d {

// ========== Schedule & Ordering ==========

enum class SBSchedule {
    Static,     // evenly partition tasks per thread (default)
    Dynamic,    // atomic counter, threads grab chunks
};

enum class SBTaskOrder {
    Logical,    // as produced by plan builder (default)
    FileOffset, // sort tasks by file_offset ascending (minimizes HDD seeks)
};

// ========== Task & Plan ==========

struct SBTask {
    uint64_t file_offset;
    uint32_t first_leaf;
    uint32_t leaf_count;
    uint64_t sb_index = 0;
};

#pragma pack(push, 4)
struct LeafOp {
    uint32_t out_base;
    uint32_t out_stride;
    uint16_t morton;
    uint8_t  param;
    uint8_t  v_inner;
    uint8_t  v_outer;
    uint8_t  pad[3];

    // Range checks: these fields are truncated from wider types.
    // morton must fit in uint16 (max 65535): super_x/leaf_x * super_y/leaf_y * super_z/leaf_z - 1
    // param, v_inner, v_outer must fit in uint8 (max 255): bounded by leaf_size
    // Call validateLeafOpRanges(header) once at startup to verify.
};
#pragma pack(pop)
static_assert(sizeof(LeafOp) == 16, "LeafOp must be 16 bytes");

inline void validateLeafOpRanges(const ERWT3DHeader& h) {
    uint64_t totalLeafs = static_cast<uint64_t>(h.super_x / h.leaf_x) *
                          (h.super_y / h.leaf_y) * (h.super_z / h.leaf_z);
    if (totalLeafs > 65536) {
        throw std::runtime_error("LeafOp::morton (uint16) cannot hold " +
            std::to_string(totalLeafs) + " leafs per superblock (max 65536)");
    }
    if (h.leaf_x > 255 || h.leaf_y > 255 || h.leaf_z > 255) {
        throw std::runtime_error("LeafOp param/v_inner/v_outer (uint8) cannot hold "
            "leaf_size > 255");
    }
}

struct SBTaskPlan {
    uint64_t superblocks_touched = 0;
    uint64_t pread_calls = 0;
    uint64_t bytes_read = 0;
    uint64_t output_bytes = 0;
    std::vector<SBTask> tasks;
    std::vector<LeafOp> leaf_ops;
    int axis = 0;
};

// ========== I/O Profile ==========

struct IOProfile {
    double plan_time_ms = 0;
    double read_time_ms = 0;
    double unpack_time_ms = 0;
    double read_time_sum_ms = 0;
    double unpack_time_sum_ms = 0;
    uint64_t superblocks_touched = 0;
    uint64_t pread_calls = 0;
    uint64_t bytes_read = 0;
    uint64_t output_bytes = 0;
    bool panel_hit = false;
};

// ========== Plan Builders (shared by SSD/HDD) ==========

SBTaskPlan buildSBPlanZ(const ERWT3DHeader& hdr, uint64_t z);
SBTaskPlan buildSBPlanY(const ERWT3DHeader& hdr, uint64_t y);
SBTaskPlan buildSBPlanX(const ERWT3DHeader& hdr, uint64_t x);

// ========== Task Ordering ==========

void sortTasksByFileOffset(SBTaskPlan& plan);

// ========== Leaf Unpacking (shared utility) ==========

void scatterDecodedLeaf(const ERWT3DHeader& hdr, const LeafOp& op,
                        const float decoded_leaf[64], float* output);

void unpackLeaves(const ERWT3DHeader& hdr, const SBTaskPlan& plan,
                  const SBTask& task, const uint8_t* sbBuf, float* output);

// ========== Internal Helpers ==========

namespace detail {
    inline uint64_t leafsPerX(const ERWT3DHeader& h) { return h.super_x / h.leaf_x; }
    inline uint64_t leafsPerY(const ERWT3DHeader& h) { return h.super_y / h.leaf_y; }
    inline uint64_t leafsPerZ(const ERWT3DHeader& h) { return h.super_z / h.leaf_z; }
    inline uint64_t sbBytes(const ERWT3DHeader& h) { return getSuperblockBytes(h); }
    inline uint64_t lfBytes(const ERWT3DHeader& h) { return getLeafBytes(h); }
} // namespace detail

} // namespace erwt3d
