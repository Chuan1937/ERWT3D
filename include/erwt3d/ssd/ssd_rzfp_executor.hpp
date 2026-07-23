#pragma once

#include "erwt3d/rzfp_format.hpp"
#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/ssd/ssd_config.hpp"

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
};

} // namespace erwt3d
