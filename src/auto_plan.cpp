#include "erwt3d/auto_plan.hpp"
#include "erwt3d/lz4_probe.hpp"
#include "erwt3d/raw_x_aux.hpp"
#include "erwt3d/sb_hdd.hpp"

#ifdef ERWT3D_HAVE_RZFP
#include "erwt3d/rzfp_auto_plan.hpp"
#include "erwt3d/rzfp_codec.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fcntl.h>
#include <iostream>
#include <random>
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

static double approxTCompositeWithDecode(
    double compressedMB, double rawMB,
    double ioBandwidthMBs, double decodeThroughputMBs,
    uint64_t preads, double seekMs, int threads)
{
    double ioTime = compressedMB / ioBandwidthMBs + static_cast<double>(preads) * seekMs / 1000.0;
    double decodeTime = rawMB / (decodeThroughputMBs * threads);
    return std::max(ioTime, decodeTime);
}

#ifdef ERWT3D_HAVE_RZFP
static double benchmarkRzfpDecode(
    int raw_fd,
    uint64_t nx, uint64_t ny, uint64_t nz,
    const RzfpCodecConfig& codecCfg,
    int threads,
    size_t targetLeaves = 512)
{
    using Clock = std::chrono::steady_clock;

    const uint64_t leaf_grid_x = (nx + 3) / 4;
    const uint64_t leaf_grid_y = (ny + 3) / 4;
    const uint64_t leaf_grid_z = (nz + 3) / 4;
    const uint64_t totalLeaves = leaf_grid_x * leaf_grid_y * leaf_grid_z;
    if (totalLeaves == 0) return 0.0;

    const uint64_t slab_x = 16;
    const uint64_t yz_floats = ny * nz;

    std::mt19937_64 rng(20260511);
    std::uniform_int_distribution<uint64_t> dist_slab(0, (nx + slab_x - 1) / slab_x - 1);
    std::uniform_int_distribution<uint64_t> dist_lx(0, leaf_grid_x > 1 ? leaf_grid_x - 2 : 0);
    std::uniform_int_distribution<uint64_t> dist_ly(0, leaf_grid_y > 1 ? leaf_grid_y - 2 : 0);
    std::uniform_int_distribution<uint64_t> dist_lz(0, leaf_grid_z > 1 ? leaf_grid_z - 2 : 0);

    std::vector<RzfpCandidate> encoded;
    encoded.reserve(targetLeaves);

    RzfpCodec codec;
    std::vector<float> slab;

    size_t attempts = 0;
    const size_t maxAttempts = targetLeaves * 5;

    while (encoded.size() < targetLeaves && attempts < maxAttempts) {
        ++attempts;
        uint64_t x_start = dist_slab(rng) * slab_x;
        if (x_start >= nx) continue;
        uint64_t current_x = std::min(slab_x, nx - x_start);

        slab.resize(current_x * yz_floats);
        uint64_t offset = x_start * yz_floats * sizeof(float);
        if (!readFullyAt(raw_fd, slab.data(), slab.size() * sizeof(float), offset)) break;

        size_t slabLeaves = 0;
        const size_t perSlab = std::min<size_t>(16, targetLeaves - encoded.size());
        while (slabLeaves < perSlab && encoded.size() < targetLeaves) {
            uint64_t lx = dist_lx(rng);
            uint64_t ly = dist_ly(rng);
            uint64_t lz = dist_lz(rng);
            if (lx * 4 + 3 >= nx || ly * 4 + 3 >= ny || lz * 4 + 3 >= nz) continue;

            float leaf[64];
            uint64_t valid_mask = 0;
            for (uint32_t z = 0; z < 4; ++z)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 4; ++x) {
                        uint32_t i = (z * 4 + y) * 4 + x;
                        uint64_t gx = lx * 4 + x, gy = ly * 4 + y, gz = lz * 4 + z;
                        if (gx < nx && gy < ny && gz < nz) {
                            valid_mask |= uint64_t{1} << i;
                            uint64_t sx = gx - x_start;
                            if (sx < current_x)
                                leaf[i] = slab[sx * yz_floats + gy * nz + gz];
                            else
                                leaf[i] = 0.0f;
                        } else {
                            leaf[i] = 0.0f;
                        }
                    }
            if (valid_mask == 0) continue;

            RzfpCandidate cand = codec.encodeBest(leaf, valid_mask, codecCfg);
            encoded.push_back(std::move(cand));
            ++slabLeaves;
        }
    }

    if (encoded.empty()) return 0.0;

    float output[64];
    constexpr int decodeRounds = 3;
    double bestTime = 1e9;

    for (int round = 0; round < decodeRounds; ++round) {
        auto db0 = Clock::now();
        for (const auto& cand : encoded) {
            codec.decode(cand, output);
        }
        auto db1 = Clock::now();
        double elapsed = std::chrono::duration<double>(db1 - db0).count();
        if (elapsed < bestTime) bestTime = elapsed;
    }

    double totalRawMiB = static_cast<double>(encoded.size() * 64 * sizeof(float)) / (1024.0 * 1024.0);
    return totalRawMiB / bestTime;
}
#endif

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

    double ioBand = 250.0;
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

    // Gray zone detection: LZ4 ratio 0.55-0.90 where format choice is uncertain
    bool inGrayZone = (lz4Ratio >= 0.55 && lz4Ratio <= 0.90);

    // LZ4 decode throughput: use benchmark if available, else default ~4000 MiB/s
    double lz4DecodeMBs = 4000.0;
    if (result.lz4_probe.decode_benchmarked && result.lz4_probe.decode_throughput_mibs > 0.0) {
        lz4DecodeMBs = result.lz4_probe.decode_throughput_mibs;
    }

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
        c.confidence = inGrayZone ? 0.5 : 0.75;
        c.uncertain = inGrayZone;
        c.reason = "LZ4 compressed + XP sidecar stride=2";

        double compressedMB = rawMB * lz4Ratio;
        double xpMB = rawMB * xpRatioEstimate;
        double hitRate = 1.0 / xpStride;

        if (inGrayZone) {
            c.predicted_x_random = hitRate * approxTCompositeWithDecode(
                    xpMB, xpMB, ioBand, lz4DecodeMBs, nx / 2, seek, threads)
                + (1.0 - hitRate) * approxTCompositeWithDecode(
                    compressedMB, rawMB, ioBand, lz4DecodeMBs, 100, seek, threads);
            c.predicted_y_random = approxTCompositeWithDecode(
                compressedMB, rawMB, ioBand, lz4DecodeMBs, 100, seek, threads);
            c.predicted_z_random = approxTCompositeWithDecode(
                compressedMB, rawMB, ioBand, lz4DecodeMBs, 100, seek, threads);
        } else {
            c.predicted_x_random = hitRate * approxTComposite(xpMB, ioBand, nx / 2, seek)
                                 + (1.0 - hitRate) * approxTComposite(compressedMB, ioBand, 100, seek);
            c.predicted_y_random = approxTComposite(compressedMB, ioBand, 100, seek);
            c.predicted_z_random = approxTComposite(compressedMB, ioBand, 100, seek);
        }
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

            // RZFP decode micro-benchmark in gray zone
            double rzfpDecodeMBs = 1000.0; // default ZFP decode throughput
            if (inGrayZone) {
                double bench = benchmarkRzfpDecode(raw_fd, nx, ny, nz,
                    rzfpCfg.main_codec_config, threads);
                if (bench > 0.0) {
                    rzfpDecodeMBs = bench;
                    result.rzfp_decode_throughput_mibs = bench;
                    result.rzfp_decode_benchmarked = true;
                }
            }

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
            c.confidence = inGrayZone ? 0.5 : 0.7;
            c.uncertain = inGrayZone;
            c.reason = "RZFP compressed (" + std::to_string(rzfpResult.sampling_rounds) + " rounds)";

            double compressedMB = rawMB * rzfpRatio;

            if (inGrayZone) {
                c.predicted_x_random = approxTCompositeWithDecode(
                    compressedMB, rawMB, ioBand, rzfpDecodeMBs, 100, seek, threads);
                c.predicted_y_random = approxTCompositeWithDecode(
                    compressedMB, rawMB, ioBand, rzfpDecodeMBs, 100, seek, threads);
                c.predicted_z_random = approxTCompositeWithDecode(
                    compressedMB, rawMB, ioBand, rzfpDecodeMBs, 100, seek, threads);
            } else {
                c.predicted_x_random = approxTComposite(compressedMB, ioBand, 100, seek);
                c.predicted_y_random = approxTComposite(compressedMB, ioBand, 100, seek);
                c.predicted_z_random = approxTComposite(compressedMB, ioBand, 100, seek);
            }
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

    if (inGrayZone) {
        std::cout << "Gray zone detected (LZ4 ratio=" << lz4Ratio << "x), decode micro-benchmarks used" << std::endl;
        if (result.lz4_probe.decode_benchmarked)
            std::cout << "  LZ4 decode: " << result.lz4_probe.decode_throughput_mibs << " MiB/s" << std::endl;
        if (result.rzfp_decode_benchmarked)
            std::cout << "  RZFP decode: " << result.rzfp_decode_throughput_mibs << " MiB/s" << std::endl;
    }

    return result;
}

} // namespace erwt3d
