#pragma once

#include "slice.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace erwt3d {

struct ContestPhase {
    std::vector<size_t> group_ids;
    uint64_t output_bytes = 0;
};

struct ContestPhasePlan {
    std::vector<ContestPhase> phases;
    bool all_outputs_deferred = false;
    uint64_t total_output_bytes = 0;
    uint64_t max_phase_output_bytes = 0;
};

ContestPhasePlan buildContestPhasePlan(
    const std::vector<uint64_t>& group_output_bytes,
    const std::vector<SliceAxis>& axes,
    const std::vector<std::string>& modes,
    uint64_t output_budget_bytes
);

bool validateContestPhasePlan(
    const ContestPhasePlan& plan,
    size_t expected_group_count,
    uint64_t output_budget_bytes,
    std::string* error
);

} // namespace erwt3d
