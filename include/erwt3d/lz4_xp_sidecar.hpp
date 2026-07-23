#pragma once

#include <cstdint>
#include <string>

namespace erwt3d {

struct Lz4XpSidecarStats {
    double compression_ratio = 0.0;
    double total_storage_ratio = 0.0;
    uint64_t sidecar_bytes = 0;
    uint32_t stride = 0;
    uint32_t plane_count = 0;
    bool embedded = false;
};

bool writeLz4XpSidecar(
    const std::string& rawPath,
    const std::string& erwtPath,
    uint64_t nx, uint64_t ny, uint64_t nz,
    uint32_t requestedStride,
    uint32_t chunkZRows = 256,
    double storageBudget = 1.50,
    bool embed = true,
    Lz4XpSidecarStats* stats = nullptr
);

} // namespace erwt3d
