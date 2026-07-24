#pragma once

#include "axis_plane.hpp"
#include "rzfp_xplane_codec.hpp"

#include <cstdint>
#include <string>

namespace erwt3d {

struct RzfpAxisPlaneWriterStats {
    PlaneAxis axis = PlaneAxis::X;
    uint64_t total_raw_bytes = 0;
    uint64_t total_compressed_bytes = 0;
    double compression_ratio = 0.0;
    double total_storage_ratio = 0.0;
    uint64_t sidecar_bytes = 0;
    uint64_t plane_count = 0;
    bool written = false;
};

// Writes an RZFP 2D-compressed axis-plane sidecar file.
// X-axis delegates to the existing writeXPlaneSidecarFile.
// Y and Z use encodeXPlane2D directly.
bool writeRzfpAxisPlaneSidecar(
    const std::string& rawPath,
    const std::string& mainPath,
    PlaneAxis axis,
    const RzfpXPlaneCodecConfig& codecConfig,
    uint64_t nx, uint64_t ny, uint64_t nz,
    int threads = 4,
    RzfpAxisPlaneWriterStats* stats = nullptr
);

} // namespace erwt3d
