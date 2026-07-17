#pragma once

#include "lz4_probe.hpp"
#include "sb_hdd.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace erwt3d {

struct FormatCandidate {
    std::string name;
    std::string main_format;       // "lz4" or "rzfp"
    std::string sidecar_format;    // "none", "lz4_xplane", "rzfp_xplane"
    uint32_t sidecar_stride = 0;

    double predicted_main_ratio = 1.0;
    double predicted_sidecar_ratio = 0.0;
    double predicted_total_ratio = 1.0;

    double predicted_t_composite = 0.0;
    double confidence = 0.0;
    bool feasible = true;
    std::string reason;
};

struct PlannerResult {
    FormatCandidate recommended;
    std::vector<FormatCandidate> alternatives;
    HDDReadWindowConfig disk_cfg;
    Lz4ProbeResult lz4_probe;
    bool rzfp_available = false;
};

PlannerResult planFormat(
    const std::string& raw_path,
    uint64_t nx, uint64_t ny, uint64_t nz,
    int threads = 8,
    double storage_budget = 1.45
);

} // namespace erwt3d
