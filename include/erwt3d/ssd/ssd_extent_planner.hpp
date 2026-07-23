#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace erwt3d {

struct SSDLeafRequest {
    uint64_t file_offset = 0;
    uint64_t record_size = 0;
    uint64_t superblock_id = 0;
    uint16_t morton = 0;
    uint16_t codec = 0;

    uint32_t logical_group = 0;
    uint32_t request_index = 0;
    float* output = nullptr;
    uint32_t out_base = 0;
    uint32_t out_stride = 0;
    uint64_t leaf_id = 0;
    bool is_xplane = false;
};

struct SSDExtent {
    uint64_t offset = 0;
    uint64_t size = 0;
    size_t first_leaf = 0;
    size_t leaf_count = 0;

    uint64_t end() const { return offset + size; }
};

struct SSDExtentPlan {
    std::vector<SSDExtent> extents;
    std::vector<SSDLeafRequest> leaves;

    uint64_t logical_requests = 0;
    uint64_t unique_leaves = 0;
    uint64_t duplicate_requests = 0;

    uint64_t requested_record_bytes = 0;
    uint64_t planned_read_bytes = 0;
    uint64_t eliminated_record_bytes = 0;

    uint64_t pread_calls = 0;
    uint64_t read_amplification_numerator = 0;
    double read_amplification = 1.0;
    uint64_t merged_gap_bytes = 0;

    size_t extent_count_before_merge = 0;
};

struct SSDExtentPlanConfig {
    uint64_t read_window_bytes = 4ULL << 20;
    uint64_t max_gap_bytes = 64ULL << 10;

    uint32_t queue_depth = 8;
    uint64_t buffer_pool_bytes = 512ULL << 20;

    double estimated_bandwidth_mb_s = 1000.0;
    double io_submission_cost_us = 10.0;
};

SSDExtentPlan buildSSDExtentPlan(
    std::vector<SSDLeafRequest> requests,
    const SSDExtentPlanConfig& config);

} // namespace erwt3d
