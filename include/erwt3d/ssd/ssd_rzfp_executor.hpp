#pragma once

#include "erwt3d/rzfp_format.hpp"
#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/rzfp_round_plan.hpp"
#include "erwt3d/ssd/ssd_config.hpp"
#include "erwt3d/ssd/ssd_extent_planner.hpp"

#include <cstdint>
#include <vector>

namespace erwt3d {

struct RzfpSSDExecProfile {
    uint64_t extent_count = 0;
    uint64_t pread_calls = 0;
    uint64_t actual_read_bytes = 0;
    uint64_t requested_record_bytes = 0;
    double read_amplification = 1.0;
    uint64_t merged_gap_bytes = 0;
    double io_time_ms = 0.0;
    double decode_time_ms = 0.0;
    double scatter_time_ms = 0.0;
    uint64_t decoded_leaf_count = 0;
    uint64_t decoded_bytes = 0;
    uint64_t scatter_elements = 0;
};

bool executeRzfpRoundSSD(
    int fd,
    const RzfpFileHeader& header,
    const std::vector<RzfpSuperblockIndex>& sbIndex,
    const std::vector<RzfpLeafDescriptor>& descriptors,
    const RzfpRoundPlan& roundPlan,
    const std::vector<RzfpReader::ContestRoundGroup>& groups,
    const SSDReadConfig& ssdCfg,
    std::vector<RzfpReader::RzfpRoundReadResult>* results,
    RzfpSSDExecProfile* profile);

} // namespace erwt3d
