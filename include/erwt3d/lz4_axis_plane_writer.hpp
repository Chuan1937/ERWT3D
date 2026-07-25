#pragma once

#include "axis_plane.hpp"
#include "lz4_xp_sidecar.hpp"

#include <cstdint>
#include <string>

namespace erwt3d {

struct Lz4AxisPlaneWriterStats {
    PlaneAxis axis = PlaneAxis::X;
    double compression_ratio = 0.0;
    double total_storage_ratio = 0.0;
    uint64_t sidecar_bytes = 0;
    uint32_t plane_count = 0;
    bool written = false;
};

// Writes an LZ4-compressed axis-plane sidecar file (.yp or .zp)
// for the given axis.  X-axis delegates to writeLz4XpSidecar.
//
// rawPath:  raw float32 file (Z-fastest layout)
// mainPath: the .erwt3d file (used to derive sidecar path only)
// axis:     Y or Z (X is delegated)
// nx,ny,nz: dimensions
// chunkElements: approximate element count per LZ4 chunk
// storageBudget: combined storage ratio limit (1.0 = raw size)
// threads:   number of worker threads
// memoryLimitMiB: writer memory budget; 0 preserves legacy behavior
bool writeLz4AxisPlaneSidecar(
    const std::string& rawPath,
    const std::string& mainPath,
    PlaneAxis axis,
    uint64_t nx, uint64_t ny, uint64_t nz,
    uint32_t chunkElements = 128 * 1024,
    double storageBudget = 1.50,
    int threads = 4,
    Lz4AxisPlaneWriterStats* stats = nullptr,
    uint64_t memoryLimitMiB = 0
);

} // namespace erwt3d
