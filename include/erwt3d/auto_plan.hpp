#pragma once

#include "lz4_probe.hpp"
#include "sb_hdd.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace erwt3d {

enum class MainFormat { Unknown, LZ4, RZFP };
enum class SidecarFormat { None, LZ4_XPlane, RZFP_XPlane };

struct FormatCandidate {
    MainFormat main_format = MainFormat::Unknown;
    SidecarFormat sidecar_format = SidecarFormat::None;
    uint32_t sidecar_stride = 0;
    bool has_raw_x_aux = false;
    std::string name;

    double main_ratio_mean = 1.0, main_ratio_lower = 1.0, main_ratio_upper = 1.0;
    double sidecar_ratio_mean = 0.0, sidecar_ratio_upper = 0.0;
    double total_ratio_mean = 1.0, total_ratio_upper = 1.0;

    double predicted_x_random = 0.0, predicted_y_random = 0.0, predicted_z_random = 0.0;
    double predicted_x_cont = 0.0, predicted_y_cont = 0.0, predicted_z_cont = 0.0;
    double predicted_t_composite = 0.0;

    double confidence = 0.0;
    bool feasible = true;
    bool uncertain = false;
    std::string reason;
};

struct PlannerResult {
    FormatCandidate recommended;
    std::vector<FormatCandidate> alternatives;
    HDDReadWindowConfig disk_cfg;
    Lz4ProbeResult lz4_probe;
    bool rzfp_available = false;
    double elapsed_seconds = 0.0;
};

PlannerResult planFormat(
    const std::string& raw_path,
    uint64_t nx, uint64_t ny, uint64_t nz,
    int threads = 8,
    double storage_budget = 1.45
);

} // namespace erwt3d
