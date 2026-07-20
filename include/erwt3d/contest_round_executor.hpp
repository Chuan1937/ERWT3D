#pragma once

#include "contest_phase_plan.hpp"
#include "memory_budget.hpp"
#include "rzfp_reader.hpp"
#include "slice.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace erwt3d {

struct ContestExecutionGroup {
    SliceAxis axis;
    std::string name;
    const std::vector<uint64_t>* indices = nullptr;
};

struct ContestExecutionProfile {
    uint64_t phase_count = 0;
    uint64_t output_buffer_bytes = 0;
    uint64_t peak_accounted_bytes = 0;

    double setup_time_ms = 0.0;
    double output_prepare_ms = 0.0;
    double read_time_ms = 0.0;
    double write_time_ms = 0.0;
    double close_time_ms = 0.0;
    double total_time_ms = 0.0;
    double wall_time_ms = 0.0;

    uint64_t logical_leaf_requests = 0;
    uint64_t duplicate_leaf_requests = 0;
    uint64_t eliminated_record_bytes = 0;
    uint64_t actual_read_bytes = 0;

    RzfpReadStrategy selected_strategy = RzfpReadStrategy::Auto;
    std::string strategy_reason;

    bool all_outputs_deferred = false;
};

bool executeContestRound(
    RzfpReader& reader,
    const RzfpFileHeader& header,
    const std::vector<ContestExecutionGroup>& groups,
    const std::string& output_dir,
    const std::string& file_prefix,
    const RzfpReaderConfig& reader_config,
    const MemoryBudget& budget,
    ContestExecutionProfile* profile
);

} // namespace erwt3d
