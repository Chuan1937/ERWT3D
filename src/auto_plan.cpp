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
#include "erwt3d/platform_io.hpp"
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

static void addCandidate(
    std::vector<FormatCandidate>& alts, FormatCandidate& c,
    const HDDReadWindowConfig& disk,
    uint64_t rawSize, double hitRate, double mainReadBytes, double mainPreads,
    double sidecarAddMB, double sidecarPreads,
    double storageBudget
) {
    if (!c.has_raw_x_aux) {
        c.total_ratio_mean = c.main_ratio_mean + c.sidecar_ratio_mean;
        c.total_ratio_upper = c.main_ratio_upper + c.sidecar_ratio_upper;
    }
    c.feasible = (c.total_ratio_upper <= storageBudget);

    if (!c.feasible) {
        c.reason += " (total ratio " + std::to_string(c.total_ratio_upper) +
                     " > budget " + std::to_string(storageBudget) + ")";
    }

    double rawMB = static_cast<double>(rawSize) / (1024.0 * 1024.0);
    double compressedMB = rawMB * c.main_ratio_mean;

    // X-axis: sidecar-hit slices go via sidecar, misses go via main file
    double xFallbackReads = std::max(1.0, rawMB * c.main_ratio_mean * (1.0 - hitRate));
    double xSidecarReads = sidecarAddMB * hitRate;
    c.predicted_x_random = (1.0 - hitRate) * approxTComposite(xFallbackReads, disk.sequential_mb_s, 100, disk.seek_ms)
                         + hitRate * approxTComposite(xSidecarReads, disk.sequential_mb_s, sidecarPreads, disk.seek_ms);

    // Y/Z axes: read from main file via HDD window
    c.predicted_y_random = approxTComposite(compressedMB, disk.sequential_mb_s, 100, disk.seek_ms);
    c.predicted_z_random = approxTComposite(compressedMB, disk.sequential_mb_s, 100, disk.seek_ms);

    c.predicted_x_cont = c.predicted_x_random * 0.08;
    c.predicted_y_cont = c.predicted_y_random * 0.06;
    c.predicted_z_cont = c.predicted_z_random * 0.06;

    c.predicted_t_composite = (c.predicted_x_random + c.predicted_y_random + c.predicted_z_random
                              + c.predicted_x_cont + c.predicted_y_cont + c.predicted_z_cont) / 6.0;

    alts.push_back(c);
}

} // namespace

static void predictRawXAuxTimes(
    FormatCandidate& candidate,
    const HDDReadWindowConfig& disk,
    uint64_t nx, uint64_t ny, uint64_t nz,
    const PlannerWorkload& workload
) {
    double planeMB = static_cast<double>(ny) * static_cast<double>(nz) * sizeof(float) / (1024.0 * 1024.0);
    double seqMBs = disk.sequential_mb_s > 0 ? disk.sequential_mb_s : 210.0;
    double seekS = (disk.seek_ms > 0 ? disk.seek_ms : 9.0) / 1000.0;

    uint64_t xRandomCount = std::min<uint64_t>(workload.x_random_slices, nx);
    uint64_t xContCount = std::min<uint64_t>(workload.x_contiguous_slices, nx);

    // X random: each plane is read via ~1 pread call in merged windows
    double randomReadMB = planeMB * static_cast<double>(xRandomCount);
    double randomSeekS = static_cast<double>(xRandomCount) * seekS;
    candidate.predicted_x_random = randomReadMB / seqMBs + randomSeekS;

    // X continuous: contiguous planes read in one window
    double contReadMB = planeMB * static_cast<double>(xContCount);
    candidate.predicted_x_cont = contReadMB / seqMBs + seekS;
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

    int raw_fd = io_open(raw_path.c_str(), O_RDONLY);
    if (raw_fd < 0) {
        result.recommended.feasible = false;
        result.recommended.reason = "cannot open raw file";
        return result;
    }

    // Calibrate disk
    result.disk_cfg = calibrateHDD(raw_fd, rawSize);
    std::cout << "Disk: seq=" << result.disk_cfg.sequential_mb_s
              << " MB/s, seek=" << result.disk_cfg.seek_ms << " ms" << std::endl;
    io_close(raw_fd);

    // LZ4 Probe
    Lz4ProbeConfig lz4_cfg;
    lz4_cfg.nx = nx; lz4_cfg.ny = ny; lz4_cfg.nz = nz;
    lz4_cfg.threads = threads;
    lz4_cfg.slabs_to_sample = 4;
    lz4_cfg.superblocks_per_slab = 64;
    result.lz4_probe = probeLz4Compression(raw_path, lz4_cfg);

    double band = result.disk_cfg.sequential_mb_s > 0 ? result.disk_cfg.sequential_mb_s : 210.0;
    double seek = result.disk_cfg.seek_ms > 0 ? result.disk_cfg.seek_ms : 9.0;

    // --- LZ4 candidates ---
    if (!result.lz4_probe.skipped) {
        {
            FormatCandidate c;
            c.name = "LZ4 main";
            c.main_format = MainFormat::LZ4;
            c.sidecar_format = SidecarFormat::None;
            c.main_ratio_mean = result.lz4_probe.main_ratio_estimate;
            c.main_ratio_lower = result.lz4_probe.main_ratio_lower;
            c.main_ratio_upper = result.lz4_probe.main_ratio_upper;
            c.confidence = 0.75;
            c.reason = "LZ4 compressed ERWT3D";
            addCandidate(result.alternatives, c, result.disk_cfg, rawSize,
                         0.0, rawMB * c.main_ratio_mean, 100, 0.0, 0, storage_budget);
        }

        double lz4SidecarRatio = result.lz4_probe.main_ratio_estimate * 0.5;
        for (uint32_t stride : {1u, 2u, 3u}) {
            FormatCandidate c;
            c.name = "LZ4 + sidecar s" + std::to_string(stride);
            c.main_format = MainFormat::LZ4;
            c.sidecar_format = SidecarFormat::LZ4_XPlane;
            c.sidecar_stride = stride;
            c.main_ratio_mean = result.lz4_probe.main_ratio_estimate;
            c.main_ratio_upper = result.lz4_probe.main_ratio_upper;
            c.sidecar_ratio_mean = lz4SidecarRatio / stride;
            c.sidecar_ratio_upper = (lz4SidecarRatio * 1.1) / stride;
            c.confidence = 0.5;
            c.reason = "LZ4 + external X-plane sidecar";
            double hitRate = (stride == 0) ? 0.0 : 1.0 / stride;
            double sidecarMB = rawMB * lz4SidecarRatio / stride;
            addCandidate(result.alternatives, c, result.disk_cfg, rawSize,
                          hitRate, rawMB * c.main_ratio_mean, 100,
                          sidecarMB, static_cast<double>(nx) / stride, storage_budget);
            }

            // LZ4 + Raw X Aux candidate
            {
                FormatCandidate c;
                c.name = "LZ4 + Raw X Aux";
                c.main_format = MainFormat::LZ4;
                c.has_raw_x_aux = true;
                c.main_ratio_mean = result.lz4_probe.main_ratio_estimate;
                c.main_ratio_upper = result.lz4_probe.main_ratio_upper;
                c.total_ratio_mean = c.main_ratio_mean + 1.0;
                c.total_ratio_upper = c.main_ratio_upper + 1.0;
                c.confidence = 0.6;
                c.reason = "LZ4 + full raw X-plane region (zero-decompress X random)";
                c.feasible = (c.total_ratio_upper <= RAW_X_AUX_HARD_LIMIT);
                c.requires_force_storage_edge =
                    (c.total_ratio_upper > RAW_X_AUX_MAX_RATIO && c.feasible);
                predictRawXAuxTimes(c, result.disk_cfg, nx, ny, nz, workload);
                double compressedMB = rawMB * c.main_ratio_mean;
                c.predicted_y_random = approxTComposite(compressedMB, result.disk_cfg.sequential_mb_s, static_cast<double>(workload.y_random_slices), result.disk_cfg.seek_ms);
                c.predicted_z_random = approxTComposite(compressedMB, result.disk_cfg.sequential_mb_s, static_cast<double>(workload.z_random_slices), result.disk_cfg.seek_ms);
                c.predicted_y_cont = c.predicted_y_random * 0.06;
                c.predicted_z_cont = c.predicted_z_random * 0.06;
                c.predicted_t_composite = (c.predicted_x_random + c.predicted_y_random + c.predicted_z_random
                                          + c.predicted_x_cont + c.predicted_y_cont + c.predicted_z_cont) / 6.0;
                result.alternatives.push_back(c);
            }
        }

    // --- RZFP candidates ---
#ifdef ERWT3D_HAVE_RZFP
    result.rzfp_available = true;
    raw_fd = io_open(raw_path.c_str(), O_RDONLY);
    if (raw_fd >= 0) {
        RzfpAutoPlanConfig rzfpCfg;
        rzfpCfg.time_limit_seconds = 300;
        rzfpCfg.soft_time_limit_seconds = 120;
        rzfpCfg.storage_limit = storage_budget;
        rzfpCfg.storage_safety_limit = storage_budget * 0.95;
        rzfpCfg.evaluate_x_sidecar = true;
        rzfpCfg.evaluate_hdd_windows = true;

        RzfpAutoPlanResult rzfpResult;
        if (runRzfpAutoPlan(raw_fd, nx, ny, nz, rzfpCfg, rzfpResult)) {
            double rzfpRatio = rzfpResult.main_ratio_estimate;
            double rzfpUpper = rzfpResult.main_ratio_upper;
            double rzfpSidecarRatio = rzfpResult.x_sidecar_ratio_estimate;

            {
                FormatCandidate c;
                c.name = "RZFP main";
                c.main_format = MainFormat::RZFP;
                c.sidecar_format = SidecarFormat::None;
                c.main_ratio_mean = rzfpRatio;
                c.main_ratio_lower = rzfpResult.main_ratio_lower;
                c.main_ratio_upper = rzfpUpper;
                c.confidence = 0.7;
                c.reason = "RZFP compressed file (" + std::to_string(rzfpResult.sampling_rounds) + " rounds)";
                addCandidate(result.alternatives, c, result.disk_cfg, rawSize,
                             0.0, rawMB * rzfpRatio, 100, 0.0, 0, storage_budget);
            }

            if (rzfpResult.enable_x_sidecar) {
                FormatCandidate c;
                c.name = "RZFP + sidecar";
                c.main_format = MainFormat::RZFP;
                c.sidecar_format = SidecarFormat::RZFP_XPlane;
                c.sidecar_stride = 1;
                c.main_ratio_mean = rzfpRatio;
                c.main_ratio_upper = rzfpUpper;
                c.sidecar_ratio_mean = rzfpSidecarRatio;
                c.sidecar_ratio_upper = rzfpResult.x_sidecar_ratio_upper;
                c.confidence = 0.5;
                c.reason = "RZFP + 2D X-plane sidecar";
addCandidate(result.alternatives, c, result.disk_cfg, rawSize,
                              1.0, rawMB * rzfpRatio, 100,
                              rawMB * rzfpSidecarRatio, static_cast<double>(nx), storage_budget);
            }

            if (rzfpResult.enable_raw_x_aux) {
                FormatCandidate c;
                c.name = "RZFP + Raw X Aux";
                c.main_format = MainFormat::RZFP;
                c.has_raw_x_aux = true;
                c.main_ratio_mean = rzfpRatio;
                c.main_ratio_upper = rzfpUpper;
                c.total_ratio_mean = rzfpRatio + 1.0;
                c.total_ratio_upper = rzfpResult.raw_x_aux_total_ratio;
                c.confidence = 0.6;
                c.reason = "RZFP + full raw X-plane region (zero-decompress X random)";
                c.feasible = (c.total_ratio_upper <= RAW_X_AUX_HARD_LIMIT);
                c.requires_force_storage_edge =
                    (c.total_ratio_upper > RAW_X_AUX_MAX_RATIO && c.feasible);
                predictRawXAuxTimes(c, result.disk_cfg, nx, ny, nz, workload);
                double compressedMB = rawMB * c.main_ratio_mean;
                c.predicted_y_random = approxTComposite(compressedMB, result.disk_cfg.sequential_mb_s, static_cast<double>(workload.y_random_slices), result.disk_cfg.seek_ms);
                c.predicted_z_random = approxTComposite(compressedMB, result.disk_cfg.sequential_mb_s, static_cast<double>(workload.z_random_slices), result.disk_cfg.seek_ms);
                c.predicted_y_cont = c.predicted_y_random * 0.06;
                c.predicted_z_cont = c.predicted_z_random * 0.06;
                c.predicted_t_composite = (c.predicted_x_random + c.predicted_y_random + c.predicted_z_random
                                          + c.predicted_x_cont + c.predicted_y_cont + c.predicted_z_cont) / 6.0;
                result.alternatives.push_back(c);
            }
        }
        io_close(raw_fd);
    }
#endif

    // Select recommended: best feasible by T_composite
    FormatCandidate* best = nullptr;
    FormatCandidate* runnerUp = nullptr;
    for (auto& c : result.alternatives) {
        if (!c.feasible) continue;
        if (!best || c.predicted_t_composite < best->predicted_t_composite) {
            runnerUp = best;
            best = &c;
        } else if (!runnerUp || c.predicted_t_composite < runnerUp->predicted_t_composite) {
            runnerUp = &c;
        }
    }

    if (best) {
        result.recommended = *best;
        if (runnerUp && best->predicted_t_composite > 0 &&
            (runnerUp->predicted_t_composite - best->predicted_t_composite) / best->predicted_t_composite < 0.10) {
            result.recommended.uncertain = true;
            result.recommended.reason += " (close to runner-up " + runnerUp->name + ", suggest benchmark both)";
        }
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
