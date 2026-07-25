#pragma once

#include "erwt3d/ssd_cold/cold_request_plan.hpp"

#include <cstdint>
#include <vector>

namespace erwt3d {
namespace ssd_cold {

struct ColdExtent {
    uint64_t file_offset = 0;
    uint64_t size = 0;
    size_t first_slab = 0;
    size_t slab_count = 0;
    int fd = -1;
};

struct ColdExtentPlanConfig {
    uint64_t max_gap_bytes = 64ULL << 10;
    uint64_t max_extent_bytes = 16ULL << 20;
    double estimated_bandwidth_mb_s = 3000.0;
    double io_submission_cost_us = 10.0;
    double max_read_amplification = 1.30;
    bool cross_section_merge = false;
    bool cross_fd_merge = false;
};

struct ColdExtentPlan {
    std::vector<ColdExtent> extents;
    const std::vector<ColdSlabRequest>* slabs = nullptr;

    uint64_t planned_read_bytes = 0;
    uint64_t gap_bytes = 0;
    uint64_t extent_count_before_merge = 0;
    double read_amplification = 1.0;
};

ColdExtentPlan buildColdExtentPlan(
    const std::vector<ColdSlabRequest>& slabs,
    const ColdExtentPlanConfig& config);

} // namespace ssd_cold
} // namespace erwt3d
