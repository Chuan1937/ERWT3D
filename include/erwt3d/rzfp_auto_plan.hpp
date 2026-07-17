#pragma once

#include "erwt3d/rzfp_codec.hpp"
#include "erwt3d/rzfp_xplane_codec.hpp"
#include "erwt3d/sb_hdd.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace erwt3d {

struct RzfpAutoPlanConfig {
    uint64_t time_limit_seconds = 600;
    uint64_t soft_time_limit_seconds = 300;
    uint64_t memory_limit_mb = 4096;

    double storage_limit = 1.50;
    double storage_safety_limit = 1.43;

    double target_composite_seconds = 0.0;
    double early_stop_margin = 0.15;

    uint32_t random_seed = 20260511;
    int max_sampling_rounds = 4;

    bool evaluate_x_sidecar = true;
    bool evaluate_hdd_windows = true;

    RzfpCodecConfig main_codec_config;
    RzfpXPlaneCodecConfig sidecar_codec_config;
};

struct RzfpAutoPlanResult {
    double main_ratio_estimate = 1.0;
    double main_ratio_lower = 1.0;
    double main_ratio_upper = 1.0;

    double x_sidecar_ratio_estimate = 1.0;
    double x_sidecar_ratio_lower = 1.0;
    double x_sidecar_ratio_upper = 1.0;

    double correlation_x = 0.0;
    double correlation_y = 0.0;
    double correlation_z = 0.0;

    double zero_fraction = 0.0;
    double near_zero_fraction = 0.0;
    double non_finite_fraction = 0.0;
    double raw_fallback_fraction = 0.0;

    double hdd_sequential_mb_s = 0.0;
    double hdd_seek_ms = 0.0;

    uint64_t read_window_bytes = 512ULL * 1024 * 1024;
    uint64_t max_gap_bytes = 8ULL * 1024 * 1024;

    bool enable_x_sidecar = false;
    bool enable_raw_x_aux = false;
    double raw_x_aux_total_ratio = 0.0;
    bool early_stopped = false;
    std::string early_stop_reason;

    double elapsed_seconds = 0.0;
    int sampling_rounds = 0;

    std::string toJson() const;
};

bool runRzfpAutoPlan(
    int raw_fd,
    uint64_t nx,
    uint64_t ny,
    uint64_t nz,
    const RzfpAutoPlanConfig& config,
    RzfpAutoPlanResult& out_result
);

} // namespace erwt3d
