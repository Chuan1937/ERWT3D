#include "erwt3d/rzfp_auto_plan.hpp"
#include "erwt3d/rzfp_raw_sampler.hpp"
#include "erwt3d/raw_layout.hpp"
#include "erwt3d/raw_x_aux.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
#include "erwt3d/platform_io.hpp"
#include <vector>

namespace erwt3d {

namespace {

using Clock = std::chrono::high_resolution_clock;

class Timer {
public:
    Timer() : start_(Clock::now()) {}

    double elapsedSeconds() const {
        return std::chrono::duration<double>(Clock::now() - start_).count();
    }

    bool over(double seconds) const {
        return elapsedSeconds() > seconds;
    }

private:
    Clock::time_point start_;
};


static uint64_t buildValidMask3D(
    uint64_t start_x,
    uint64_t start_y,
    uint64_t start_z,
    uint64_t nx,
    uint64_t ny,
    uint64_t nz
) {
    uint64_t mask = 0;
    for (uint32_t z = 0; z < 4; ++z) {
        for (uint32_t y = 0; y < 4; ++y) {
            for (uint32_t x = 0; x < 4; ++x) {
                const uint32_t i = (z * 4 + y) * 4 + x;
                if (start_x + x < nx && start_y + y < ny && start_z + z < nz) {
                    mask |= uint64_t{1} << i;
                }
            }
        }
    }
    return mask;
}

struct LeafSample {
    uint64_t start_x;
    uint64_t start_y;
    uint64_t start_z;
    uint64_t valid_mask;
};

static void extractLeafFromSlab(
    const float* slab,
    uint64_t slab_x_start,
    uint64_t slab_x_count,
    uint64_t ny,
    uint64_t nz,
    const LeafSample& s,
    float leaf[64]
) {
    const uint64_t yz_stride = ny * nz;
    for (uint32_t z = 0; z < 4; ++z) {
        for (uint32_t y = 0; y < 4; ++y) {
            for (uint32_t x = 0; x < 4; ++x) {
                const uint32_t i = (z * 4 + y) * 4 + x;
                if ((s.valid_mask & (uint64_t{1} << i)) == 0) {
                    leaf[i] = 0.0f;
                    continue;
                }
                const uint64_t gx = s.start_x + x;
                const uint64_t gy = s.start_y + y;
                const uint64_t gz = s.start_z + z;
                const uint64_t slab_x = gx - slab_x_start;
                if (slab_x >= slab_x_count) {
                    leaf[i] = 0.0f;
                    continue;
                }
                leaf[i] = slab[slab_x * yz_stride + gy * nz + gz];
            }
        }
    }
}

struct MainSample {
    uint32_t total_bytes = 0;
    bool raw_fallback = false;
};

static std::vector<MainSample> sampleMainLeaves(
    int fd,
    uint64_t nx,
    uint64_t ny,
    uint64_t nz,
    const RzfpCodecConfig& cfg,
    std::mt19937_64& rng,
    size_t target_samples,
    const Timer& timer,
    double time_limit_seconds,
    std::vector<float>& correlation_values
) {
    std::vector<MainSample> results;
    results.reserve(target_samples);

    const uint64_t leaf_grid_x = (nx + 3) / 4;
    const uint64_t leaf_grid_y = (ny + 3) / 4;
    const uint64_t leaf_grid_z = (nz + 3) / 4;

    const uint64_t slab_x = 16;
    const uint64_t yz_floats = ny * nz;

    std::vector<uint64_t> slab_starts;
    const uint64_t num_slabs = (nx + slab_x - 1) / slab_x;
    // Stratified slab positions covering the volume.
    const std::vector<double> stratified_frac = {0.0, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875, 1.0};
    for (double f : stratified_frac) {
        uint64_t s = static_cast<uint64_t>(f * (num_slabs > 0 ? num_slabs - 1 : 0)) * slab_x;
        if (s >= nx) continue;
        slab_starts.push_back(s);
    }
    std::uniform_int_distribution<uint64_t> dist_slab(0, num_slabs > 0 ? num_slabs - 1 : 0);
    while (slab_starts.size() < 32 && slab_starts.size() < num_slabs) {
        uint64_t s = dist_slab(rng) * slab_x;
        if (s >= nx) continue;
        slab_starts.push_back(s);
    }
    std::sort(slab_starts.begin(), slab_starts.end());
    slab_starts.erase(std::unique(slab_starts.begin(), slab_starts.end()), slab_starts.end());

    const size_t per_slab = std::max<size_t>(1, target_samples / slab_starts.size());

    RzfpCodec codec;
    std::vector<float> slab;

    std::uniform_int_distribution<uint64_t> dist_y(0, leaf_grid_y > 0 ? leaf_grid_y - 1 : 0);
    std::uniform_int_distribution<uint64_t> dist_z(0, leaf_grid_z > 0 ? leaf_grid_z - 1 : 0);

    correlation_values.clear();
    correlation_values.reserve(target_samples * 4);

    for (uint64_t x_start : slab_starts) {
        if (timer.over(time_limit_seconds)) break;
        if (results.size() >= target_samples) break;

        const uint64_t current_x = std::min<uint64_t>(slab_x, nx - x_start);
        const uint64_t x_end = x_start + current_x;
        const uint64_t lx_start = x_start / 4;
        const uint64_t lx_end = x_end / 4;
        if (lx_start >= lx_end) continue;

        slab.resize(current_x * yz_floats);
        const uint64_t offset = x_start * yz_floats * sizeof(float);
        if (!readFullyAt(fd, slab.data(), slab.size() * sizeof(float), offset)) {
            break;
        }

        std::uniform_int_distribution<uint64_t> dist_local_x(lx_start, lx_end - 1);

        size_t slab_generated = 0;
        size_t slab_attempts = 0;
        const size_t max_slab_attempts = per_slab * 5;
        while (slab_generated < per_slab && slab_attempts < max_slab_attempts && results.size() < target_samples) {
            ++slab_attempts;

            LeafSample s;
            s.start_x = dist_local_x(rng) * 4;
            s.start_y = dist_y(rng) * 4;
            s.start_z = dist_z(rng) * 4;
            s.valid_mask = buildValidMask3D(s.start_x, s.start_y, s.start_z, nx, ny, nz);
            if (s.valid_mask == 0) continue;

            float leaf[64];
            extractLeafFromSlab(slab.data(), x_start, current_x, ny, nz, s, leaf);

            RzfpCandidate cand = codec.encodeBest(leaf, s.valid_mask, cfg);
            MainSample ms;
            ms.total_bytes = cand.serialized_size;
            ms.raw_fallback = (cand.codec == RzfpLeafCodec::RawFloat32);
            results.push_back(ms);
            ++slab_generated;

            if (s.start_x + 1 < nx && s.start_y + 1 < ny && s.start_z + 1 < nz) {
                const uint64_t base = 0;
                const float v = leaf[base];
                if (std::isfinite(v)) {
                    correlation_values.push_back(v);
                    correlation_values.push_back(leaf[base + 1]);
                    correlation_values.push_back(leaf[base + 4]);
                    correlation_values.push_back(leaf[base + 16]);
                }
            }
        }
    }

    return results;
}

static double pearsonCorrelation(const std::vector<float>& pairs) {
    if (pairs.size() < 4) return 0.0;
    const size_t n = pairs.size() / 2;
    if (n < 3) return 0.0;

    double sum_x = 0.0, sum_y = 0.0;
    for (size_t i = 0; i < n; ++i) {
        sum_x += pairs[2 * i];
        sum_y += pairs[2 * i + 1];
    }
    const double mean_x = sum_x / n;
    const double mean_y = sum_y / n;

    double sxx = 0.0, syy = 0.0, sxy = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double dx = pairs[2 * i] - mean_x;
        const double dy = pairs[2 * i + 1] - mean_y;
        sxx += dx * dx;
        syy += dy * dy;
        sxy += dx * dy;
    }

    if (sxx <= 0.0 || syy <= 0.0) return 0.0;
    return sxy / std::sqrt(sxx * syy);
}

static double bootstrapUpper(const std::vector<double>& values, double confidence) {
    if (values.empty()) return 1.0;
    const size_t n = values.size();
    std::mt19937_64 rng(20260511);
    std::uniform_int_distribution<size_t> dist(0, n - 1);

    const int iterations = 1000;
    std::vector<double> means;
    means.reserve(iterations);

    for (int it = 0; it < iterations; ++it) {
        double sum = 0.0;
        for (size_t i = 0; i < n; ++i) {
            sum += values[dist(rng)];
        }
        means.push_back(sum / n);
    }

    std::sort(means.begin(), means.end());
    const size_t idx = static_cast<size_t>(confidence * (means.size() - 1));
    return means[std::min(idx, means.size() - 1)];
}

static double bootstrapLower(const std::vector<double>& values, double confidence) {
    if (values.empty()) return 1.0;
    const size_t n = values.size();
    std::mt19937_64 rng(20260511);
    std::uniform_int_distribution<size_t> dist(0, n - 1);

    const int iterations = 1000;
    std::vector<double> means;
    means.reserve(iterations);

    for (int it = 0; it < iterations; ++it) {
        double sum = 0.0;
        for (size_t i = 0; i < n; ++i) {
            sum += values[dist(rng)];
        }
        means.push_back(sum / n);
    }

    std::sort(means.begin(), means.end());
    const size_t idx = static_cast<size_t>((1.0 - confidence) * (means.size() - 1));
    return means[idx];
}

static double measureHDDBandwidth(int fd, uint64_t file_size, uint64_t test_bytes) {
    if (fd < 0 || file_size == 0) return 0.0;
    const uint64_t to_read = std::min(test_bytes, file_size);
    if (to_read == 0) return 0.0;

    std::vector<uint8_t> buffer(64 * 1024 * 1024);
    const uint64_t start_offset = 0;

    auto t0 = Clock::now();
    uint64_t remaining = to_read;
    uint64_t offset = start_offset;
    while (remaining > 0) {
        const uint64_t chunk = std::min<uint64_t>(buffer.size(), remaining);
        if (!readFullyAt(fd, buffer.data(), chunk, offset)) return 0.0;
        remaining -= chunk;
        offset += chunk;
    }
    auto t1 = Clock::now();

    const double seconds = std::chrono::duration<double>(t1 - t0).count();
    if (seconds <= 0.0) return 0.0;
    return (to_read / seconds) / (1024.0 * 1024.0);
}

static double estimateXSpeedup(double main_ratio, double sidecar_ratio) {
    if (sidecar_ratio <= 0.0) return 1.0;
    const double main_amplification = 4.0;
    return (main_ratio * main_amplification) / sidecar_ratio;
}

static void chooseHDDWindow(
    uint64_t raw_size,
    double hdd_mb_s,
    uint64_t& read_window,
    uint64_t& max_gap
) {
    if (raw_size >= 20ULL * 1024 * 1024 * 1024 && hdd_mb_s < 400.0) {
        read_window = 512ULL * 1024 * 1024;
        max_gap = 8ULL * 1024 * 1024;
    } else if (raw_size >= 5ULL * 1024 * 1024 * 1024) {
        read_window = 256ULL * 1024 * 1024;
        max_gap = 4ULL * 1024 * 1024;
    } else {
        read_window = 128ULL * 1024 * 1024;
        max_gap = 2ULL * 1024 * 1024;
    }
}

static void computeGlobalStats(
    int fd,
    uint64_t nx,
    uint64_t ny,
    uint64_t nz,
    std::mt19937_64& rng,
    size_t point_samples,
    double& zero_fraction,
    double& near_zero_fraction,
    double& non_finite_fraction
) {
    zero_fraction = 0.0;
    near_zero_fraction = 0.0;
    non_finite_fraction = 0.0;

    if (fd < 0 || nx * ny * nz == 0) return;

    std::uniform_int_distribution<uint64_t> dist_y(0, ny - 1);
    std::uniform_int_distribution<uint64_t> dist_z(0, nz - 1);

    std::vector<float> values;
    values.reserve(point_samples);

    const uint64_t yz_floats = ny * nz;
    const uint64_t slab_x = std::max<uint64_t>(1, std::min<uint64_t>(nx / 4, 32));
    std::vector<float> slab;

    size_t zeros = 0;
    size_t near_zeros = 0;
    size_t non_finite = 0;

    const std::vector<uint64_t> slab_starts = {
        0ULL,
        nx / 4,
        nx / 2,
        3 * nx / 4,
        nx > slab_x ? nx - slab_x : 0
    };

    for (uint64_t x_start : slab_starts) {
        if (x_start >= nx) continue;
        const uint64_t current_x = std::min(slab_x, nx - x_start);
        slab.resize(current_x * yz_floats);
        const uint64_t offset = x_start * yz_floats * sizeof(float);
        if (!readFullyAt(fd, slab.data(), slab.size() * sizeof(float), offset)) return;

        const size_t per_slab = point_samples / slab_starts.size();
        std::uniform_int_distribution<uint64_t> dist_x(0, current_x - 1);
        for (size_t i = 0; i < per_slab; ++i) {
            const uint64_t local_x = dist_x(rng);
            const uint64_t y = dist_y(rng);
            const uint64_t z = dist_z(rng);
            const float v = slab[rawOffsetZFastest(local_x, y, z, ny, nz)];
            values.push_back(std::abs(v));
            if (v == 0.0f) ++zeros;
            if (!std::isfinite(v)) ++non_finite;
        }
    }

    if (!values.empty()) {
        std::sort(values.begin(), values.end());
        const size_t p1_idx = std::min<size_t>(values.size() * 0.01, values.size() - 1);
        const double threshold = std::max(1e-30, static_cast<double>(values[p1_idx]) * 1e-3);
        for (float av : values) {
            if (av < threshold) ++near_zeros;
        }
        zero_fraction = static_cast<double>(zeros) / values.size();
        near_zero_fraction = static_cast<double>(near_zeros) / values.size();
        non_finite_fraction = static_cast<double>(non_finite) / values.size();
    }
}

} // namespace

std::string RzfpAutoPlanResult::toJson() const {
    std::ostringstream json;
    json << std::fixed << std::setprecision(6);
    json << "{\n";
    json << "  \"main_ratio\": {\n";
    json << "    \"estimate\": " << main_ratio_estimate << ",\n";
    json << "    \"lower\": " << main_ratio_lower << ",\n";
    json << "    \"upper\": " << main_ratio_upper << "\n";
    json << "  },\n";
    json << "  \"x_sidecar_ratio\": {\n";
    json << "    \"estimate\": " << x_sidecar_ratio_estimate << ",\n";
    json << "    \"lower\": " << x_sidecar_ratio_lower << ",\n";
    json << "    \"upper\": " << x_sidecar_ratio_upper << "\n";
    json << "  },\n";
    json << "  \"data\": {\n";
    json << "    \"zero_fraction\": " << zero_fraction << ",\n";
    json << "    \"near_zero_fraction\": " << near_zero_fraction << ",\n";
    json << "    \"non_finite_fraction\": " << non_finite_fraction << ",\n";
    json << "    \"correlation_x\": " << correlation_x << ",\n";
    json << "    \"correlation_y\": " << correlation_y << ",\n";
    json << "    \"correlation_z\": " << correlation_z << ",\n";
    json << "    \"raw_fallback_fraction\": " << raw_fallback_fraction << "\n";
    json << "  },\n";
    json << "  \"hdd\": {\n";
    json << "    \"sequential_mb_s\": " << hdd_sequential_mb_s << ",\n";
    json << "    \"seek_ms\": " << hdd_seek_ms << ",\n";
    json << "    \"read_window_bytes\": " << read_window_bytes << ",\n";
    json << "    \"max_gap_bytes\": " << max_gap_bytes << "\n";
    json << "  },\n";
    json << "  \"sidecar\": {\n";
    json << "    \"enabled\": " << (enable_x_sidecar ? "true" : "false") << ",\n";
    json << "    \"predicted_x_speedup\": " << estimateXSpeedup(main_ratio_estimate, x_sidecar_ratio_estimate) << "\n";
    json << "  },\n";
    json << "  \"raw_x_aux\": {\n";
    json << "    \"enabled\": " << (enable_raw_x_aux ? "true" : "false") << ",\n";
    json << "    \"total_ratio_upper\": " << raw_x_aux_total_ratio << "\n";
    json << "  },\n";
    json << "  \"elapsed_seconds\": " << elapsed_seconds << ",\n";
    json << "  \"sampling_rounds\": " << sampling_rounds << ",\n";
    json << "  \"early_stopped\": " << (early_stopped ? "true" : "false") << ",\n";
    json << "  \"early_stop_reason\": \"" << early_stop_reason << "\"\n";
    json << "}\n";
    return json.str();
}

bool runRzfpAutoPlan(
    int raw_fd,
    uint64_t nx,
    uint64_t ny,
    uint64_t nz,
    const RzfpAutoPlanConfig& config,
    RzfpAutoPlanResult& out_result
) {
    if (raw_fd < 0 || nx == 0 || ny == 0 || nz == 0) return false;

    out_result = RzfpAutoPlanResult();
    Timer timer;
    std::mt19937_64 rng(config.random_seed);

    const uint64_t raw_size = nx * ny * nz * sizeof(float);
    const uint64_t total_leaves = ((nx + 3) / 4) * ((ny + 3) / 4) * ((nz + 3) / 4);
    const double raw_per_leaf = 256.0;
    (void)total_leaves;

    RzfpCodecConfig main_cfg = config.main_codec_config;
    if (main_cfg.error.contest_bound <= 0.0) {
        main_cfg.error.contest_bound = 1e-3;
        main_cfg.error.internal_bound = 7.5e-4;
        main_cfg.error.policy = RelativeErrorPolicy::Strict;
    }

    RzfpXPlaneCodecConfig sidecar_cfg = config.sidecar_codec_config;
    if (sidecar_cfg.error.contest_bound <= 0.0) {
        sidecar_cfg.error.contest_bound = 1e-3;
        sidecar_cfg.error.internal_bound = 7.5e-4;
        sidecar_cfg.error.policy = RelativeErrorPolicy::Strict;
    }

    std::vector<MainSample> all_main_samples;
    std::vector<double> all_sidecar_ratios;
    std::vector<float> correlation_values;

    const size_t round1_main = 20000;
    const size_t round2_main = 50000;
    const size_t round_more = 50000;

    const int round1_x_planes = 6;
    const int round2_x_planes = 10;

    bool stop = false;
    int round = 0;

    while (!stop && round < config.max_sampling_rounds && !timer.over(config.time_limit_seconds)) {
        ++round;

        size_t main_target = (round == 1) ? round1_main : round2_main;
        if (round > 2) main_target = round_more;

        std::vector<float> round_correlations;
        auto main_samples = sampleMainLeaves(
            raw_fd, nx, ny, nz, main_cfg, rng, main_target,
            timer, config.time_limit_seconds, round_correlations
        );

        all_main_samples.insert(all_main_samples.end(), main_samples.begin(), main_samples.end());
        correlation_values.insert(correlation_values.end(), round_correlations.begin(), round_correlations.end());

        if (config.evaluate_x_sidecar && !timer.over(config.time_limit_seconds)) {
            const int x_plane_count = (round == 1) ? round1_x_planes : round2_x_planes;
            std::vector<uint64_t> x_positions;
            x_positions.reserve(x_plane_count);

            x_positions.push_back(0);
            if (nx > 1) x_positions.push_back(nx - 1);
            if (nx > 4) x_positions.push_back(nx / 4);
            if (nx > 2) x_positions.push_back(nx / 2);
            if (nx > 4) x_positions.push_back(3 * nx / 4);

            std::uniform_int_distribution<uint64_t> dist_x(0, nx > 0 ? nx - 1 : 0);
            while (static_cast<int>(x_positions.size()) < x_plane_count) {
                x_positions.push_back(dist_x(rng));
            }

            const uint64_t chunk_z = std::max<uint64_t>(4, std::min<uint64_t>(nz / 4, 32));
            std::vector<RawZRange> z_ranges;
            if (nz <= chunk_z) {
                z_ranges.push_back({0, nz});
            } else {
                z_ranges.push_back({0, chunk_z});
                z_ranges.push_back({nz / 3, chunk_z});
                z_ranges.push_back({2 * nz / 3, chunk_z});
                z_ranges.push_back({nz - chunk_z, chunk_z});
            }

            std::vector<SampledXPlane> planes;
            if (sampleXPlanesFromRaw(raw_fd, nx, ny, nz, x_positions, z_ranges, planes)) {
                for (const auto& plane : planes) {
                    if (plane.data.empty()) continue;
                    auto record = encodeXPlane2D(plane.data.data(), ny, plane.z_count, sidecar_cfg);
                    const double raw_bytes = static_cast<double>(ny * plane.z_count * sizeof(float));
                    all_sidecar_ratios.push_back(static_cast<double>(record.size()) / raw_bytes);
                }
            }
        }

        if (all_main_samples.size() >= 1000) {
            std::vector<double> ratios;
            ratios.reserve(all_main_samples.size());
            for (const auto& s : all_main_samples) ratios.push_back(static_cast<double>(s.total_bytes) / raw_per_leaf);
            out_result.main_ratio_estimate = std::accumulate(ratios.begin(), ratios.end(), 0.0) / ratios.size();
            out_result.main_ratio_upper = bootstrapUpper(ratios, 0.95);
            out_result.main_ratio_lower = bootstrapLower(ratios, 0.95);
        }

        if (!all_sidecar_ratios.empty()) {
            out_result.x_sidecar_ratio_estimate = std::accumulate(all_sidecar_ratios.begin(), all_sidecar_ratios.end(), 0.0) / all_sidecar_ratios.size();
            out_result.x_sidecar_ratio_upper = bootstrapUpper(all_sidecar_ratios, 0.95);
            out_result.x_sidecar_ratio_lower = bootstrapLower(all_sidecar_ratios, 0.95);
        }

        size_t raw_fallbacks = 0;
        for (const auto& s : all_main_samples) {
            if (s.raw_fallback) ++raw_fallbacks;
        }
        out_result.raw_fallback_fraction = all_main_samples.empty() ? 0.0 : static_cast<double>(raw_fallbacks) / all_main_samples.size();

        const double combined_upper = out_result.main_ratio_upper + out_result.x_sidecar_ratio_upper;
        const double speedup = estimateXSpeedup(out_result.main_ratio_estimate, out_result.x_sidecar_ratio_estimate);

        if (round >= 1) {
            if (out_result.main_ratio_upper <= config.storage_safety_limit) {
                if (!config.evaluate_x_sidecar || combined_upper > config.storage_safety_limit || speedup < 2.0) {
                    if (round >= 2 || timer.over(config.soft_time_limit_seconds)) {
                        out_result.enable_x_sidecar = false;
                        out_result.early_stopped = true;
                        out_result.early_stop_reason = "main file reaches target with safe margin; sidecar not beneficial";
                        stop = true;
                    }
                } else {
                    out_result.enable_x_sidecar = true;
                    out_result.early_stopped = true;
                    out_result.early_stop_reason = "sidecar beneficial and within storage budget";
                    stop = true;
                }
            } else if (out_result.main_ratio_upper > config.storage_limit) {
                if (round >= config.max_sampling_rounds || timer.over(config.time_limit_seconds)) {
                    out_result.early_stopped = true;
                    out_result.early_stop_reason = "main file alone exceeds storage limit; plan infeasible with current codec";
                    stop = true;
                }
            }
        }
    }

    if (!stop && timer.over(config.soft_time_limit_seconds)) {
        out_result.early_stopped = true;
        out_result.early_stop_reason = "soft time limit reached";
    }

    out_result.sampling_rounds = round;

    if (correlation_values.size() >= 12) {
        std::vector<float> pairs_x;
        std::vector<float> pairs_y;
        std::vector<float> pairs_z;
        for (size_t i = 0; i + 3 < correlation_values.size(); i += 4) {
            pairs_x.push_back(correlation_values[i]);
            pairs_x.push_back(correlation_values[i + 1]);
            pairs_y.push_back(correlation_values[i]);
            pairs_y.push_back(correlation_values[i + 2]);
            pairs_z.push_back(correlation_values[i]);
            pairs_z.push_back(correlation_values[i + 3]);
        }
        out_result.correlation_x = pearsonCorrelation(pairs_x);
        out_result.correlation_y = pearsonCorrelation(pairs_y);
        out_result.correlation_z = pearsonCorrelation(pairs_z);
    }

    computeGlobalStats(raw_fd, nx, ny, nz, rng, 20000,
                       out_result.zero_fraction,
                       out_result.near_zero_fraction,
                       out_result.non_finite_fraction);

    if (config.evaluate_hdd_windows) {
        out_result.hdd_sequential_mb_s = measureHDDBandwidth(raw_fd, raw_size, 256ULL * 1024 * 1024);
        out_result.hdd_seek_ms = 9.0;
        chooseHDDWindow(raw_size, out_result.hdd_sequential_mb_s,
                        out_result.read_window_bytes, out_result.max_gap_bytes);
    }

    const double combined_upper = out_result.main_ratio_upper + out_result.x_sidecar_ratio_upper;
    const double speedup = estimateXSpeedup(out_result.main_ratio_estimate, out_result.x_sidecar_ratio_estimate);
    if (config.evaluate_x_sidecar && combined_upper <= config.storage_safety_limit && speedup >= 2.0) {
        out_result.enable_x_sidecar = true;
    } else {
        out_result.enable_x_sidecar = false;
    }

    // Raw X aux: deterministic storage cost = main ratio + 1.0 + alignment overhead
    const double alignmentFraction =
        static_cast<double>(RAW_X_AUX_ALIGN - 1) / static_cast<double>(raw_size);
    const double rawXAuxTotalUpper =
        out_result.main_ratio_upper + 1.0 + alignmentFraction;
    out_result.raw_x_aux_total_ratio = rawXAuxTotalUpper;

    // Prefer raw X aux over sidecar when it fits in budget
    if (rawXAuxTotalUpper <= config.storage_safety_limit) {
        out_result.enable_raw_x_aux = true;
        out_result.enable_x_sidecar = false; // raw X aux supersedes sidecar
    } else if (rawXAuxTotalUpper <= config.storage_limit) {
        out_result.enable_raw_x_aux = true;
        // Still fits under hard limit but needs --force-storage-edge
    } else {
        out_result.enable_raw_x_aux = false;
    }

    out_result.elapsed_seconds = timer.elapsedSeconds();
    return true;
}

} // namespace erwt3d
