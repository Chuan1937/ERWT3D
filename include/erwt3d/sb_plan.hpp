#pragma once

#include "format.hpp"
#include <cstdint>
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
};

struct SBTaskPlan {
    uint64_t superblocks_touched = 0;
    uint64_t pread_calls = 0;
    uint64_t bytes_read = 0;
    uint64_t output_bytes = 0;
    std::vector<SBTask> tasks;
    // leaf_data: 4 uint64_t per leaf + output packing info
    // [mortar, leafD0, leafD1, param | out_base_hi<<32|out_base_lo, out_stride, vx, vy]
    std::vector<uint64_t> leaf_data;
    std::vector<uint32_t> leaf_out; // [out_base, out_stride, v_inner, v_outer]
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
