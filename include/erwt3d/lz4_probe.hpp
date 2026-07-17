#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace erwt3d {

struct Lz4ProbeResult {
    double main_ratio_estimate = 1.0;
    double main_ratio_lower = 1.0;
    double main_ratio_upper = 1.0;
    double compressed_block_fraction = 0.0;
    uint64_t sampled_superblocks = 0;
    uint64_t sampled_raw_bytes = 0;
    double elapsed_seconds = 0.0;
    bool skipped = false;
    std::string skip_reason;
};

struct Lz4ProbeConfig {
    uint64_t nx = 0, ny = 0, nz = 0;
    uint32_t super_x = 64, super_y = 64, super_z = 64;
    uint32_t leaf_x = 4, leaf_y = 4, leaf_z = 4;
    uint32_t slab_stride = 1;
    uint32_t slabs_to_sample = 4;
    uint32_t superblocks_per_slab = 32;
    int threads = 1;
    double skip_threshold = 0.90;
};

Lz4ProbeResult probeLz4Compression(const std::string& raw_path,
                                    const Lz4ProbeConfig& config);

} // namespace erwt3d
