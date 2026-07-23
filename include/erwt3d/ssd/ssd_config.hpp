#pragma once

#include <cstdint>

namespace erwt3d {

struct SSDReadConfig {
    int read_threads = 4;
    int decode_threads = 8;

    uint64_t read_window_bytes = 4ULL << 20;
    uint64_t max_gap_bytes = 64ULL << 10;

    uint32_t queue_depth = 8;
    uint64_t buffer_pool_bytes = 512ULL << 20;

    bool fuse_decode_scatter = true;
    bool use_fadvise = true;
    bool pin_workers = false;
};

struct SSDWriterConfig {
    int writer_threads = 2;
    uint64_t queue_high_water_bytes = 768ULL << 20;
    uint64_t queue_low_water_bytes = 256ULL << 20;

    bool use_ftruncate = true;
    bool use_posix_fallocate = false;
    bool write_whole_slice = true;
};

} // namespace erwt3d
