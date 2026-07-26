#pragma once

#include "erwt3d/ssd_cold/cold_profile.hpp"
#include "erwt3d/ssd_cold/cold_io_engine.hpp"
#include "erwt3d/contest_positions.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace erwt3d {
namespace ssd_cold {

struct RzfpColdConfig {
    int read_threads = 4;
    int decode_threads = 6;
    int write_threads = 2;

    uint64_t max_gap_bytes = 64ULL << 10;
    uint64_t max_extent_bytes = 4ULL << 20;
    uint64_t memory_limit_mb = 8192;

    std::string write_mode = "auto";

    ColdIOBackend io_backend = ColdIOBackend::Auto;
    int queue_depth = 8;
    bool use_direct_io = false;
};

unsigned detectAvailableCpuCount();

unsigned resolveColdDecodeThreads(int requestedThreads);

bool executeRzfpAxisLeafColdSSD(
    const std::string& filePath,
    const ContestPositions& positions,
    const std::string& outputDir,
    const RzfpColdConfig& config,
    ColdProfile* profile = nullptr);

} // namespace ssd_cold
} // namespace erwt3d
