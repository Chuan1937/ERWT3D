#include "erwt3d/auto_plan.hpp"
#include "erwt3d/lz4_probe.hpp"
#include "erwt3d/raw_x_aux.hpp"
#include "erwt3d/sb_hdd.hpp"

#ifdef ERWT3D_HAVE_RZFP
#include "erwt3d/rzfp_auto_plan.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>
#include <vector>

namespace erwt3d {

namespace {

static double approxTComposite(double readMB, double bandwidthMBs,
                                uint64_t preads, double seekMs) {
    if (bandwidthMBs <= 0.0) return 1e9;
    double ioTime = readMB / bandwidthMBs;
    double seekTime = static_cast<double>(preads) * seekMs / 1000.0;
    return ioTime + seekTime;
}

}

PlannerResult planFormat(
    const std::string& raw_path,
    uint64_t nx, uint64_t ny, uint64_t nz,
    int threads,
    double storage_budget,
    const PlannerWorkload& workload
) {
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();
    PlannerResult result;

    uint64_t xy = 0, xyz = 0, rawSize = 0;
    if (!checkedMulU64(nx, ny, xy) || !checkedMulU64(xy, nz, xyz) ||
        !checkedMulU64(xyz, sizeof(float), rawSize)) {
        result.recommended.feasible = false;
        result.recommended.reason = "raw size overflow";
        return result;
    }
    double rawMB = static_cast<double>(rawSize) / (1024.0 * 1024.0);

    result.disk_cfg.sequential_mb_s = 250.0;
    result.disk_cfg.seek_ms = 10.0;
    result.disk_cfg.read_window_bytes = 128ULL * 1024 * 1024;
    result.disk_cfg.max_gap_bytes = 8ULL * 1024 * 1024;

    double band = 250.0;
    double seek = 10.0;

    // --- LZ4 Probe ---
    Lz4ProbeConfig lz4_cfg;
    lz4_cfg.nx = nx; lz4_cfg.ny = ny; lz4_cfg.nz = nz;
    lz4_cfg.threads = threads;
    lz4_cfg.slabs_to_sample = 4;
    lz4_cfg.superblocks_per_slab = 64;
    result.lz4_probe = probeLz4Compression(raw_path, lz4_cfg);

    double lz4Ratio = result.lz4_probe.main_ratio_estimate;
    double lz4Upper = result.lz4_probe.main_ratio_upper;

    // XP stride=2 sidecar ratio estimate: ~0.5 * main_ratio / stride
    double xpStride = 2.0;
    double xpRatioEstimate = lz4Ratio * 0.5 / xpStride;
    double xpRatioUpper = lz4Upper * 0.55 / xpStride;

    // --- Candidate A: LZ4 + XP stride=2 ---
    {
        FormatCandidate c;
        c.name = "LZ4 + XP stride=2";
        c.main_format = MainFormat::LZ4;
        c.sidecar_format = SidecarFormat::LZ4_XPlane;
        c.sidecar_stride = 2;
        c.main_ratio_mean = lz4Ratio;
        c.main_ratio_lower = result.lz4_probe.main_ratio_lower;
        c.main_ratio_upper = lz4Upper;
        c.sidecar_ratio_mean = xpRatioEstimate;
        c.sidecar_ratio_upper = xpRatioUpper;
        c.total_ratio_mean = lz4Ratio + xpRatioEstimate;
        c.total_ratio_upper = lz4Upper + xpRatioUpper;
        c.feasible = (c.total_ratio_upper <= storage_budget);
        c.confidence = 0.75;
        c.reason = "LZ4 compressed + XP sidecar stride=2";

        double compressedMB = rawMB * lz4Ratio;
        double xpMB = rawMB * xpRatioEstimate;
        double hitRate = 1.0 / xpStride;

        c.predicted_x_random = hitRate * approxTComposite(xpMB, band, nx / 2, seek)
                             + (1.0 - hitRate) * approxTComposite(compressedMB, band, 100, seek);
        c.predicted_y_random = approxTComposite(compressedMB, band, 100, seek);
        c.predicted_z_random = approxTComposite(compressedMB, band, 100, seek);
        c.predicted_x_cont = c.predicted_x_random * 0.08;
        c.predicted_y_cont = c.predicted_y_random * 0.06;
        c.predicted_z_cont = c.predicted_z_random * 0.06;
        c.predicted_t_composite = (c.predicted_x_random + c.predicted_y_random + c.predicted_z_random
                                  + c.predicted_x_cont + c.predicted_y_cont + c.predicted_z_cont) / 6.0;

        if (!c.feasible) {
            c.reason += " (total ratio " + std::to_string(c.total_ratio_upper) +
                        " > budget " + std::to_string(storage_budget) + ")";
        }
        result.alternatives.push_back(c);
    }

    // --- Candidate B: Pure RZFP ---
#ifdef ERWT3D_HAVE_RZFP
    result.rzfp_available = true;
    int raw_fd = open(raw_path.c_str(), O_RDONLY);
    if (raw_fd >= 0) {
        RzfpAutoPlanConfig rzfpCfg;
        rzfpCfg.time_limit_seconds = 300;
        rzfpCfg.soft_time_limit_seconds = 120;
        rzfpCfg.storage_limit = storage_budget;
        rzfpCfg.storage_safety_limit = storage_budget * 0.95;
        rzfpCfg.evaluate_x_sidecar = false;
        rzfpCfg.evaluate_hdd_windows = false;
        rzfpCfg.main_codec_config.error.policy = RelativeErrorPolicy::Strict;
        rzfpCfg.main_codec_config.error.contest_bound = 1e-3;
        rzfpCfg.main_codec_config.error.internal_bound = 7.5e-4;

        RzfpAutoPlanResult rzfpResult;
        if (runRzfpAutoPlan(raw_fd, nx, ny, nz, rzfpCfg, rzfpResult)) {
            double rzfpRatio = rzfpResult.main_ratio_estimate;
            double rzfpUpper = rzfpResult.main_ratio_upper;

            FormatCandidate c;
            c.name = "RZFP";
            c.main_format = MainFormat::RZFP;
            c.sidecar_format = SidecarFormat::None;
            c.main_ratio_mean = rzfpRatio;
            c.main_ratio_lower = rzfpResult.main_ratio_lower;
            c.main_ratio_upper = rzfpUpper;
            c.total_ratio_mean = rzfpRatio;
            c.total_ratio_upper = rzfpUpper;
            c.feasible = (c.total_ratio_upper <= storage_budget);
            c.confidence = 0.7;
            c.reason = "RZFP compressed (" + std::to_string(rzfpResult.sampling_rounds) + " rounds)";

            double compressedMB = rawMB * rzfpRatio;
            c.predicted_x_random = approxTComposite(compressedMB, band, 100, seek);
            c.predicted_y_random = approxTComposite(compressedMB, band, 100, seek);
            c.predicted_z_random = approxTComposite(compressedMB, band, 100, seek);
            c.predicted_x_cont = c.predicted_x_random * 0.08;
            c.predicted_y_cont = c.predicted_y_random * 0.06;
            c.predicted_z_cont = c.predicted_z_random * 0.06;
            c.predicted_t_composite = (c.predicted_x_random + c.predicted_y_random + c.predicted_z_random
                                      + c.predicted_x_cont + c.predicted_y_cont + c.predicted_z_cont) / 6.0;

            if (!c.feasible) {
                c.reason += " (total ratio " + std::to_string(c.total_ratio_upper) +
                            " > budget " + std::to_string(storage_budget) + ")";
            }
            result.alternatives.push_back(c);
        }
        close(raw_fd);
    }
#endif

    // Select recommended: best feasible by predicted T_composite
    FormatCandidate* best = nullptr;
    for (auto& c : result.alternatives) {
        if (!c.feasible) continue;
        if (!best || c.predicted_t_composite < best->predicted_t_composite) {
            best = &c;
        }
    }

    if (best) {
        result.recommended = *best;
    } else {
        result.recommended.feasible = false;
        result.recommended.reason = "no feasible format under budget " + std::to_string(storage_budget);
    }

    auto t1 = Clock::now();
    result.elapsed_seconds = std::chrono::duration<double>(t1 - t0).count();

    std::cout << "Planning complete in " << result.elapsed_seconds << "s, "
              << result.alternatives.size() << " candidates evaluated" << std::endl;

    return result;
}

} // namespace erwt3d
