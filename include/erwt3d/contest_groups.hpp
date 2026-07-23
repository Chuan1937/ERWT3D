#pragma once

#include "contest_positions.hpp"
#include "slice.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace erwt3d {

using ContestReadBatchFunction = std::function<bool(
    SliceAxis axis,
    const std::vector<uint64_t>& indices,
    std::vector<std::vector<float>>& outputs
)>;

struct ContestGroupTiming {
    double time_ms = 0.0;
    uint64_t slice_count = 0;
    uint64_t total_bytes = 0;
};

struct ContestUnifiedProfile {
    ContestGroupTiming x_random;
    ContestGroupTiming y_random;
    ContestGroupTiming z_random;
    ContestGroupTiming x_continuous;
    ContestGroupTiming y_continuous;
    ContestGroupTiming z_continuous;

    double t_x_ms = 0.0;
    double t_y_ms = 0.0;
    double t_z_ms = 0.0;
    double t_composite_ms = 0.0;
    double process_e2e_ms = 0.0;

    uint64_t output_file_count = 0;
    uint64_t output_total_bytes = 0;
};

bool executeContestGroups(
    const ContestPositions& positions,
    const std::string& outputDir,
    uint64_t nx,
    uint64_t ny,
    uint64_t nz,
    const ContestReadBatchFunction& reader,
    ContestUnifiedProfile* profile
);

} // namespace erwt3d
