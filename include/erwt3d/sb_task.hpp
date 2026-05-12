#pragma once
#include "format.hpp"
#include <cstdint>
#include <vector>

namespace erwt3d {

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

SBTaskPlan buildSBPlanZ(const ERWT3DHeader& hdr, uint64_t z);
SBTaskPlan buildSBPlanY(const ERWT3DHeader& hdr, uint64_t y);
SBTaskPlan buildSBPlanX(const ERWT3DHeader& hdr, uint64_t x);

bool executeSBPlanSerial(int fd, const SBTaskPlan& plan, const ERWT3DHeader& hdr,
                         float* output, IOProfile* profile);
bool executeSBPlanParallelRead(int fd, const SBTaskPlan& plan, const ERWT3DHeader& hdr,
                                float* output, int numThreads, IOProfile* profile);

bool tryReadSliceXPanels(int fd, const ERWT3DHeader& hdr, uint64_t x,
                          float* output, IOProfile* profile);

bool tryReadSliceXPanelsParallel(int fd, const ERWT3DHeader& hdr, uint64_t x,
                                  float* output, int numThreads, IOProfile* profile);

} // namespace erwt3d
