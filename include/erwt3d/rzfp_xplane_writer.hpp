#pragma once

#include "rzfp_xplane_codec.hpp"

#include <cstdint>
#include <string>

namespace erwt3d {

struct RzfpXPlaneWriterStats {
    uint64_t total_raw_bytes = 0;
    uint64_t total_compressed_bytes = 0;
    double compression_ratio = 0.0;
    uint64_t plane_count = 0;
};

bool writeXPlaneSidecarFile(
    const std::string& raw_path,
    const std::string& output_path,
    const RzfpXPlaneCodecConfig& cfg,
    uint64_t nx,
    uint64_t ny,
    uint64_t nz,
    int threads,
    RzfpXPlaneWriterStats* out_stats = nullptr
);

} // namespace erwt3d
