#include "erwt3d/auto_plan.hpp"
#include "erwt3d/lz4_probe.hpp"
#include "erwt3d/sb_hdd.hpp"

#ifdef ERWT3D_HAVE_RZFP
#include "erwt3d/rzfp_auto_plan.hpp"
#include "erwt3d/rzfp_writer.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

namespace erwt3d {

PlannerResult planFormat(
    const std::string& raw_path,
    uint64_t nx, uint64_t ny, uint64_t nz,
    int threads,
    double storage_budget
) {
    PlannerResult result;

    // Open raw file for calibration
    int raw_fd = open(raw_path.c_str(), O_RDONLY);
    if (raw_fd < 0) {
        result.recommended.feasible = false;
        result.recommended.reason = "cannot open raw file";
        return result;
    }

    uint64_t raw_size = nx * ny * nz * sizeof(float);

    // Calibrate disk
    result.disk_cfg = calibrateHDD(raw_fd, raw_size);
    std::cout << "Disk: seq=" << result.disk_cfg.sequential_mb_s
              << " MB/s, seek=" << result.disk_cfg.seek_ms << " ms" << std::endl;

    close(raw_fd);

    // LZ4 probe
    Lz4ProbeConfig lz4_cfg;
    lz4_cfg.nx = nx; lz4_cfg.ny = ny; lz4_cfg.nz = nz;
    lz4_cfg.threads = threads;
    lz4_cfg.slabs_to_sample = 4;
    lz4_cfg.superblocks_per_slab = 64;
    result.lz4_probe = probeLz4Compression(raw_path, lz4_cfg);

    // LZ4 candidate
    {
        FormatCandidate c;
        c.name = "LZ4 main";
        c.main_format = "lz4";
        c.sidecar_format = "none";
        c.predicted_main_ratio = result.lz4_probe.main_ratio_estimate;
        c.predicted_total_ratio = c.predicted_main_ratio;
        c.feasible = (c.predicted_total_ratio <= storage_budget);
        c.reason = "LZ4 compressed ERWT3D file";
        if (!c.feasible) c.reason += " (ratio exceeds budget)";

        // Rough T_composite estimate: sequential scan all 3 axes
        double rawMB = static_cast<double>(raw_size) / (1024.0 * 1024.0);
        double compressedMB = rawMB * c.predicted_main_ratio;
        if (result.disk_cfg.sequential_mb_s > 0) {
            c.predicted_t_composite = (3.0 * compressedMB) / result.disk_cfg.sequential_mb_s;
        }
        c.confidence = 0.7;
        result.alternatives.push_back(c);
    }

    // LZ4 + sidecar candidates
    if (result.lz4_probe.main_ratio_estimate <= storage_budget) {
        for (uint32_t stride : {1u, 2u}) {
            FormatCandidate c;
            c.main_format = "lz4";
            c.sidecar_format = "lz4_xplane";
            c.sidecar_stride = stride;
            c.name = "LZ4 + sidecar s" + std::to_string(stride);
            c.predicted_main_ratio = result.lz4_probe.main_ratio_estimate;
            // Sidecar ratio estimated from the LZ4 probe's compression fraction
            double sidecarOverhead = result.lz4_probe.main_ratio_estimate * 0.5;
            c.predicted_sidecar_ratio = sidecarOverhead / stride;
            c.predicted_total_ratio = c.predicted_main_ratio + c.predicted_sidecar_ratio;
            c.feasible = (c.predicted_total_ratio <= storage_budget);
            c.confidence = 0.4;
            if (c.feasible) {
                double rawMB = static_cast<double>(raw_size) / (1024.0 * 1024.0);
                double sidecarBenefit = 10.0; // rough seconds saved on X-random
                c.predicted_t_composite = std::max(0.1, result.alternatives.back().predicted_t_composite - sidecarBenefit / stride);
            }
            result.alternatives.push_back(c);
        }
    }

#ifdef ERWT3D_HAVE_RZFP
    result.rzfp_available = true;

    // RZFP candidate
    {
        FormatCandidate c;
        c.name = "RZFP main";
        c.main_format = "rzfp";
        c.sidecar_format = "none";
        double estRatio = 1.0;
        {
            // Quick RZFP size estimate: average from the LZ4 probe
            // Actual RZFP auto-plan would give better estimate
            c.predicted_main_ratio = 0.6;  // conservative default
        }
        c.predicted_total_ratio = c.predicted_main_ratio;
        c.feasible = (c.predicted_total_ratio <= storage_budget);
        c.reason = "RZFP compressed file";
        c.confidence = 0.3;
        result.alternatives.push_back(c);
    }
#endif

    // Select recommended: best feasible by T_composite
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
        result.recommended.reason = "no feasible format found under budget " + std::to_string(storage_budget);
    }

    return result;
}

} // namespace erwt3d
