#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace erwt3d {

struct Lz4ProbeResult {
    double main_ratio_estimate = 1.0;
    double main_ratio_lower = 1.0;
    double main_ratio_upper = 1.0;
    double main_ratio_median = 1.0;
    double main_ratio_stddev = 0.0;
    double main_ratio_p10 = 1.0;
    double main_ratio_p90 = 1.0;
    double compressed_block_fraction = 0.0;
    uint64_t sampled_superblocks = 0;
    uint64_t sampled_raw_bytes = 0;
    uint32_t slabs_sampled = 0;
    double elapsed_seconds = 0.0;
    bool skipped = false;
    bool adaptive = false;
    std::string skip_reason;
    std::vector<double> ratios;  // per-superblock ratios (probe-internal, for stats)
};

struct Lz4ProbeConfig {
    uint64_t nx = 0, ny = 0, nz = 0;
    uint32_t super_x = 64, super_y = 64, super_z = 64;
    uint32_t leaf_x = 4, leaf_y = 4, leaf_z = 4;
    uint32_t slabs_to_sample = 4;
    uint32_t superblocks_per_slab = 32;
    int threads = 1;
    double skip_threshold = 0.90;
    bool adaptive_sampling = true;
    uint32_t max_total_superblocks = 2048;
};

Lz4ProbeResult probeLz4Compression(const std::string& raw_path,
                                    const Lz4ProbeConfig& config);

} // namespace erwt3d
