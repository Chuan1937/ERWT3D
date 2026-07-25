#pragma once

#include "erwt3d/ssd_cold/cold_request_plan.hpp"

#include <cstdint>
#include <vector>

namespace erwt3d {
namespace ssd_cold {

struct ColdExtent {
    uint64_t file_offset = 0;
    uint64_t size = 0;
    size_t first_record = 0;
    size_t record_count = 0;
};

struct ColdExtentPlanConfig {
    uint64_t max_gap_bytes = 64ULL << 10;
    uint64_t max_extent_bytes = 4ULL << 20;
    double estimated_bandwidth_mb_s = 3000.0;
    double io_submission_cost_us = 10.0;
    double max_read_amplification = 1.30;
    bool cross_section_merge = false;
};

struct ColdExtentPlan {
    std::vector<ColdExtent> extents;
    const std::vector<ColdLeafRecord>* records = nullptr;

    uint64_t planned_read_bytes = 0;
    uint64_t gap_bytes = 0;
    uint64_t extent_count_before_merge = 0;
    double read_amplification = 1.0;
};

ColdExtentPlan buildColdExtentPlan(
    const std::vector<ColdLeafRecord>& records,
    const ColdExtentPlanConfig& config);

} // namespace ssd_cold
} // namespace erwt3d
