#include "erwt3d/rzfp_codec.hpp"
#include "erwt3d/relative_error.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace erwt3d {

namespace {

struct ProbeOptions {
    std::string raw_path;
    uint64_t nx = 0;
    uint64_t ny = 0;
    uint64_t nz = 0;
    size_t sample_leaves = 1000000;
    std::string sample_mode = "stratified";
    double internal_rel_bound = 7.5e-4;
    double contest_rel_bound = 1e-3;
    std::string error_policy = "strict";
    std::string exception_counts_str = "0,1,2,4,8,16";
    std::string precisions_str = "12,14,16,18,20,22,24";
    std::string fill_modes_str = "zero,mean";
    int threads = 0;
    uint64_t slab_z = 16;
    std::string csv_path;
    std::string json_path;
};

struct LeafSample {
    uint64_t leaf_id;
    uint64_t start_x;
    uint64_t start_y;
    uint64_t start_z;
    uint64_t valid_mask;
};

struct LeafResult {
    uint64_t leaf_id;
    uint32_t valid_count;
    uint32_t mandatory_count;
    RzfpLeafCodec codec;
    int16_t parameter;
    RzfpExceptionFill fill_mode;
    uint32_t exception_count;
    uint32_t zfp_payload_bytes;
    uint32_t record_bytes;
    uint32_t total_bytes;
    double encode_ns;
    double decode_ns;
    double max_abs_error;
    double max_rel_error;
    uint32_t violation_count;
    bool raw_fallback;
    double min_abs_non_exception;
    double max_abs;
    uint32_t zero_count;
    uint32_t subnormal_count;
    uint32_t special_count;
};

static uint64_t buildValidMask(
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
                if (
                    start_x + x < nx &&
                    start_y + y < ny &&
                    start_z + z < nz
                ) {
                    mask |= uint64_t{1} << i;
                }
            }
        }
    }
    return mask;
}

static std::vector<uint8_t> parseExceptionCounts(const std::string& s) {
    std::vector<uint8_t> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        int v = std::stoi(tok);
        if (v < 0 || v > 64) throw std::invalid_argument("invalid exception count");
        out.push_back(static_cast<uint8_t>(v));
    }
    return out;
}

static std::vector<uint8_t> parsePrecisions(const std::string& s) {
    std::vector<uint8_t> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        int v = std::stoi(tok);
        if (v < 1 || v > 64) throw std::invalid_argument("invalid precision");
        out.push_back(static_cast<uint8_t>(v));
    }
    return out;
}

static std::vector<RzfpExceptionFill> parseFillModes(const std::string& s) {
    std::vector<RzfpExceptionFill> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (tok == "zero") out.push_back(RzfpExceptionFill::Zero);
        else if (tok == "mean") out.push_back(RzfpExceptionFill::Mean);
        else throw std::invalid_argument("invalid fill mode");
    }
    return out;
}

static RelativeErrorPolicy parsePolicy(const std::string& s) {
    if (s == "strict") return RelativeErrorPolicy::Strict;
    if (s == "legacy") return RelativeErrorPolicy::Legacy;
    throw std::invalid_argument("invalid error policy");
}

static const char* codecName(RzfpLeafCodec c) {
    switch (c) {
        case RzfpLeafCodec::RawFloat32: return "RawFloat32";
        case RzfpLeafCodec::ConstantZero: return "ConstantZero";
        case RzfpLeafCodec::ConstantValue: return "ConstantValue";
        case RzfpLeafCodec::ZfpAccuracy: return "ZfpAccuracy";
        case RzfpLeafCodec::ZfpAccuracyExceptions: return "ZfpAccuracyExceptions";
        case RzfpLeafCodec::ZfpPrecision: return "ZfpPrecision";
    }
    return "Unknown";
}

static const char* fillName(RzfpExceptionFill f) {
    return f == RzfpExceptionFill::Zero ? "Zero" : "Mean";
}

static bool readFullyAt(int fd, void* buffer, size_t bytes, uint64_t offset) {
    auto* dst = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    while (done < bytes) {
        ssize_t n = pread(fd, dst + done, bytes - done, static_cast<off_t>(offset + done));
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return true;
}

static void extractLeaf(
    const float* slab,
    uint64_t slab_x_start,
    uint64_t slab_x_count,
    uint64_t ny,
    uint64_t nz,
    const LeafSample& sample,
    float leaf[64]
) {
    const uint64_t yz_stride = ny * nz;
    for (uint32_t z = 0; z < 4; ++z) {
        for (uint32_t y = 0; y < 4; ++y) {
            for (uint32_t x = 0; x < 4; ++x) {
                const uint32_t i = (z * 4 + y) * 4 + x;
                if ((sample.valid_mask & (uint64_t{1} << i)) == 0) {
                    leaf[i] = 0.0f;
                    continue;
                }
                const uint64_t gx = sample.start_x + x;
                const uint64_t gy = sample.start_y + y;
                const uint64_t gz = sample.start_z + z;
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

static LeafResult processOneLeaf(
    const float leaf[64],
    const LeafSample& sample,
    const RzfpCodecConfig& codec_cfg
) {
    LeafResult r{};
    r.leaf_id = sample.leaf_id;
    r.valid_count = static_cast<uint32_t>(__builtin_popcountll(sample.valid_mask));

    uint64_t mandatory_mask = 0;
    for (uint32_t i = 0; i < 64; ++i) {
        if ((sample.valid_mask & (uint64_t{1} << i)) == 0) continue;
        const float v = leaf[i];
        if (v == 0.0f) ++r.zero_count;
        if (!std::isfinite(v)) ++r.special_count;
        if (std::fpclassify(v) == FP_SUBNORMAL) ++r.subnormal_count;
        if (v == 0.0f || !std::isfinite(v) || std::fpclassify(v) == FP_SUBNORMAL) {
            mandatory_mask |= uint64_t{1} << i;
        }
        r.max_abs = std::max(r.max_abs, std::abs(static_cast<double>(v)));
    }
    r.mandatory_count = static_cast<uint32_t>(__builtin_popcountll(mandatory_mask));

    r.min_abs_non_exception = std::numeric_limits<double>::infinity();
    for (uint32_t i = 0; i < 64; ++i) {
        const uint64_t bit = uint64_t{1} << i;
        if ((sample.valid_mask & bit) == 0 || (mandatory_mask & bit) != 0) continue;
        r.min_abs_non_exception = std::min(r.min_abs_non_exception, std::abs(static_cast<double>(leaf[i])));
    }

    RzfpCodec codec;
    auto t0 = std::chrono::high_resolution_clock::now();
    RzfpCandidate cand = codec.encodeBest(leaf, sample.valid_mask, codec_cfg);
    auto t1 = std::chrono::high_resolution_clock::now();
    r.encode_ns = std::chrono::duration<double, std::nano>(t1 - t0).count();

    r.codec = cand.codec;
    r.parameter = cand.parameter;
    r.fill_mode = cand.fill_mode;
    r.exception_count = cand.exception_count;
    r.zfp_payload_bytes = cand.zfp_payload_size;
    r.record_bytes = static_cast<uint32_t>(cand.payload.size());
    r.total_bytes = cand.serialized_size;
    r.raw_fallback = (cand.codec == RzfpLeafCodec::RawFloat32);
    r.violation_count = cand.error_stats.violation_count;
    r.max_abs_error = cand.error_stats.max_absolute_error;
    r.max_rel_error = cand.error_stats.max_relative_error;

    float decoded[64];
    t0 = std::chrono::high_resolution_clock::now();
    bool dec_ok = codec.decode(cand, decoded);
    t1 = std::chrono::high_resolution_clock::now();
    r.decode_ns = std::chrono::duration<double, std::nano>(t1 - t0).count();

    if (!dec_ok) {
        r.violation_count = std::numeric_limits<uint32_t>::max();
    }

    return r;
}

static void printUsage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " --raw PATH --nx N --ny N --nz N [options]\n\n"
        << "Options:\n"
        << "  --raw PATH                 Raw float32 file (required)\n"
        << "  --nx N                     X dimension (required)\n"
        << "  --ny N                     Y dimension (required)\n"
        << "  --nz N                     Z dimension (required)\n"
        << "  --sample-leaves N          Number of leaves to probe (default: 1000000)\n"
        << "  --sample-mode MODE         stratified (default)\n"
        << "  --internal-rel-bound V     Internal relative bound (default: 0.00075)\n"
        << "  --contest-rel-bound V      Contest relative bound (default: 0.001)\n"
        << "  --error-policy strict|legacy (default: strict)\n"
        << "  --exception-counts LIST    e.g. 0,1,2,4,8,16\n"
        << "  --precisions LIST          e.g. 12,14,16,18,20,22,24\n"
        << "  --fill-modes LIST          zero,mean\n"
        << "  --threads N                Worker threads (0=auto)\n"
        << "  --slab-z N                 Z slab height for reads (default: 16)\n"
        << "  --csv PATH                 Per-leaf CSV output\n"
        << "  --json PATH                JSON summary output\n";
}

static ProbeOptions parseOptions(int argc, char* argv[]) {
    ProbeOptions opt;
    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> const char* {
            if (i + 1 < argc) return argv[++i];
            std::cerr << "Error: " << argv[i] << " requires a value\n";
            std::exit(1);
            return nullptr;
        };
        if (std::strcmp(argv[i], "--raw") == 0) opt.raw_path = next();
        else if (std::strcmp(argv[i], "--nx") == 0) opt.nx = std::stoull(next());
        else if (std::strcmp(argv[i], "--ny") == 0) opt.ny = std::stoull(next());
        else if (std::strcmp(argv[i], "--nz") == 0) opt.nz = std::stoull(next());
        else if (std::strcmp(argv[i], "--sample-leaves") == 0) opt.sample_leaves = std::stoull(next());
        else if (std::strcmp(argv[i], "--sample-mode") == 0) opt.sample_mode = next();
        else if (std::strcmp(argv[i], "--internal-rel-bound") == 0) opt.internal_rel_bound = std::stod(next());
        else if (std::strcmp(argv[i], "--contest-rel-bound") == 0) opt.contest_rel_bound = std::stod(next());
        else if (std::strcmp(argv[i], "--error-policy") == 0) opt.error_policy = next();
        else if (std::strcmp(argv[i], "--exception-counts") == 0) opt.exception_counts_str = next();
        else if (std::strcmp(argv[i], "--precisions") == 0) opt.precisions_str = next();
        else if (std::strcmp(argv[i], "--fill-modes") == 0) opt.fill_modes_str = next();
        else if (std::strcmp(argv[i], "--threads") == 0) opt.threads = std::stoi(next());
        else if (std::strcmp(argv[i], "--slab-z") == 0) opt.slab_z = std::stoull(next());
        else if (std::strcmp(argv[i], "--csv") == 0) opt.csv_path = next();
        else if (std::strcmp(argv[i], "--json") == 0) opt.json_path = next();
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]); std::exit(0);
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            printUsage(argv[0]); std::exit(1);
        }
    }
    return opt;
}

} // namespace

} // namespace erwt3d

int main(int argc, char* argv[]) {
    using namespace erwt3d;

    ProbeOptions opt = parseOptions(argc, argv);
    if (opt.raw_path.empty() || opt.nx == 0 || opt.ny == 0 || opt.nz == 0) {
        std::cerr << "Error: --raw, --nx, --ny, --nz are required\n";
        printUsage(argv[0]);
        return 1;
    }

    int threads = opt.threads;
    if (threads <= 0) {
        unsigned hw = std::thread::hardware_concurrency();
        threads = static_cast<int>(std::min(std::max(1u, hw / 2), 8u));
    }

    RzfpCodecConfig codec_cfg;
    codec_cfg.error.policy = parsePolicy(opt.error_policy);
    codec_cfg.error.contest_bound = opt.contest_rel_bound;
    codec_cfg.error.internal_bound = opt.internal_rel_bound;
    codec_cfg.optional_exception_counts = parseExceptionCounts(opt.exception_counts_str);
    codec_cfg.precisions = parsePrecisions(opt.precisions_str);
    codec_cfg.fill_modes = parseFillModes(opt.fill_modes_str);

    const uint64_t leaf_grid_x = (opt.nx + 3) / 4;
    const uint64_t leaf_grid_y = (opt.ny + 3) / 4;
    const uint64_t leaf_grid_z = (opt.nz + 3) / 4;
    const uint64_t total_leaves = leaf_grid_x * leaf_grid_y * leaf_grid_z;
    const uint64_t raw_size = opt.nx * opt.ny * opt.nz * sizeof(float);

    std::mt19937_64 rng(20260511);

    int raw_fd = open(opt.raw_path.c_str(), O_RDONLY);
    if (raw_fd < 0) {
        std::cerr << "Error: cannot open raw file: " << opt.raw_path << std::endl;
        return 1;
    }

    std::ofstream csv_file;
    std::mutex csv_mutex;
    if (!opt.csv_path.empty()) {
        csv_file.open(opt.csv_path);
        csv_file << "dataset,leaf_id,valid_count,mandatory_exception_count,selected_codec,"
                    "parameter,fill_mode,exception_count,zfp_payload_bytes,record_bytes,"
                    "descriptor_bytes,total_bytes,compression_ratio,encode_ns,decode_ns,"
                    "max_abs_error,max_rel_error,violation_count,raw_fallback,"
                    "min_abs_non_exception,max_abs,dynamic_range,zero_count,subnormal_count,"
                    "special_count\n";
    }

    std::vector<LeafResult> all_results;
    std::mutex result_mutex;
    std::atomic<uint64_t> total_violations{0};
    std::atomic<uint64_t> total_valid_points{0};
    std::atomic<uint64_t> total_raw_fallback{0};
    std::atomic<uint64_t> total_record_bytes{0};
    std::atomic<uint64_t> total_total_bytes{0};
    std::atomic<uint64_t> codec_counts[6] = {};
    std::atomic<uint64_t> exc_hist[33] = {};

    auto worker = [&](
        const std::vector<LeafSample>& samples,
        const float* slab,
        uint64_t slab_x_start,
        uint64_t slab_x_count,
        size_t begin,
        size_t end
    ) {
        RzfpCodec codec;
        for (size_t i = begin; i < end; ++i) {
            const auto& s = samples[i];
            float leaf[64];
            extractLeaf(slab, slab_x_start, slab_x_count, opt.ny, opt.nz, s, leaf);
            LeafResult r = processOneLeaf(leaf, s, codec_cfg);

            total_violations.fetch_add(r.violation_count);
            total_valid_points.fetch_add(r.valid_count);
            if (r.raw_fallback) total_raw_fallback.fetch_add(1);
            total_record_bytes.fetch_add(r.record_bytes);
            total_total_bytes.fetch_add(r.total_bytes);
            codec_counts[static_cast<uint8_t>(r.codec)].fetch_add(1);
            if (r.exception_count <= 32) {
                exc_hist[r.exception_count].fetch_add(1);
            }

            if (!opt.csv_path.empty()) {
                std::lock_guard<std::mutex> lock(csv_mutex);
                csv_file << opt.raw_path << "," << r.leaf_id << "," << r.valid_count << ","
                         << r.mandatory_count << "," << codecName(r.codec) << ","
                         << static_cast<int>(r.parameter) << "," << fillName(r.fill_mode) << ","
                         << r.exception_count << "," << r.zfp_payload_bytes << ","
                         << r.record_bytes << ",2," << r.total_bytes << ","
                         << std::fixed << std::setprecision(6)
                         << (static_cast<double>(r.total_bytes) / 256.0) << ","
                         << r.encode_ns << "," << r.decode_ns << ","
                         << r.max_abs_error << "," << r.max_rel_error << ","
                         << r.violation_count << "," << (r.raw_fallback ? "1" : "0") << ","
                         << r.min_abs_non_exception << "," << r.max_abs << ","
                         << (r.min_abs_non_exception > 0.0 ? r.max_abs / r.min_abs_non_exception : 0.0) << ","
                         << r.zero_count << "," << r.subnormal_count << "," << r.special_count << "\n";
            }

            {
                std::lock_guard<std::mutex> lock(result_mutex);
                all_results.push_back(r);
            }
        }
    };

    const std::vector<int> stratified_percent = {0, 10, 25, 50, 75, 90, 100};
    const size_t regions = stratified_percent.size();
    const size_t base_per_region = opt.sample_leaves / regions;
    size_t remainder = opt.sample_leaves % regions;

    auto start_time = std::chrono::high_resolution_clock::now();

    for (size_t ri = 0; ri < regions; ++ri) {
        size_t region_samples = base_per_region + (ri < remainder ? 1 : 0);
        if (region_samples == 0) continue;

        uint64_t x_center = static_cast<uint64_t>(
            stratified_percent[ri] * static_cast<long double>(opt.nx) / 100.0L
        );
        uint64_t x_start = x_center;
        if (opt.nx > opt.slab_z && x_start + opt.slab_z > opt.nx) {
            x_start = opt.nx - opt.slab_z;
        }
        uint64_t current_x = std::min<uint64_t>(opt.slab_z, opt.nx - x_start);
        if (current_x == 0) continue;

        const uint64_t yz_floats = opt.ny * opt.nz;
        const uint64_t slab_floats = yz_floats * current_x;
        std::vector<float> slab(slab_floats);
        const uint64_t offset = x_start * yz_floats * sizeof(float);
        if (!readFullyAt(raw_fd, slab.data(), slab_floats * sizeof(float), offset)) {
            std::cerr << "Error reading raw X-slab at x=" << x_start << std::endl;
            close(raw_fd);
            return 1;
        }

        std::vector<LeafSample> samples;
        samples.reserve(region_samples);
        const uint64_t x_low = x_start / 4;
        const uint64_t x_high = std::min<uint64_t>((x_start + current_x + 3) / 4, leaf_grid_x);
        std::uniform_int_distribution<uint64_t> dist_x(
            x_low,
            x_high > x_low ? x_high - 1 : x_low
        );
        std::uniform_int_distribution<uint64_t> dist_y(0, leaf_grid_y > 0 ? leaf_grid_y - 1 : 0);
        std::uniform_int_distribution<uint64_t> dist_z(0, leaf_grid_z > 0 ? leaf_grid_z - 1 : 0);

        for (size_t s = 0; s < region_samples; ++s) {
            uint64_t lx = dist_x(rng);
            uint64_t ly = dist_y(rng);
            uint64_t lz = dist_z(rng);
            LeafSample sample;
            sample.start_x = lx * 4;
            sample.start_y = ly * 4;
            sample.start_z = lz * 4;
            sample.leaf_id = (lz * leaf_grid_y + ly) * leaf_grid_x + lx;
            sample.valid_mask = buildValidMask(
                sample.start_x, sample.start_y, sample.start_z,
                opt.nx, opt.ny, opt.nz
            );
            samples.push_back(sample);
        }

        std::vector<std::thread> workers;
        size_t chunk = (samples.size() + threads - 1) / threads;
        for (int t = 0; t < threads; ++t) {
            size_t b = t * chunk;
            size_t e = std::min(b + chunk, samples.size());
            if (b >= e) continue;
            workers.emplace_back(worker, std::cref(samples), slab.data(), x_start, current_x, b, e);
        }
        for (auto& w : workers) w.join();

        std::cout << "\rProbe progress: region " << (ri + 1) << "/" << regions
                  << " samples=" << all_results.size() << "/" << opt.sample_leaves
                  << std::flush;
    }
    std::cout << std::endl;
    close(raw_fd);

    if (!opt.csv_path.empty()) {
        csv_file.close();
    }

    const size_t n = all_results.size();
    if (n == 0) {
        std::cerr << "No samples processed" << std::endl;
        return 1;
    }

    std::vector<uint32_t> total_sizes;
    total_sizes.reserve(n);
    double max_rel = 0.0;
    double total_encode_ns = 0.0, total_decode_ns = 0.0;
    for (const auto& r : all_results) {
        total_sizes.push_back(r.total_bytes);
        max_rel = std::max(max_rel, r.max_rel_error);
        total_encode_ns += r.encode_ns;
        total_decode_ns += r.decode_ns;
    }
    std::sort(total_sizes.begin(), total_sizes.end());
    auto pct = [&](double p) -> uint32_t {
        size_t idx = static_cast<size_t>(p * (total_sizes.size() - 1));
        if (idx >= total_sizes.size()) idx = total_sizes.size() - 1;
        return total_sizes[idx];
    };

    const double avg_total_bytes = static_cast<double>(total_total_bytes.load()) / n;
    const double avg_record_bytes = static_cast<double>(total_record_bytes.load()) / n;
    const double projected_ratio = (avg_total_bytes * total_leaves) / raw_size;
    const double raw_fallback_ratio = static_cast<double>(total_raw_fallback.load()) / n;

    const double elapsed_s = std::chrono::duration<double>(
        std::chrono::high_resolution_clock::now() - start_time
    ).count();
    const uint64_t total_valid = total_valid_points.load();
    const double encode_mbps = (total_valid * sizeof(float)) / (total_encode_ns * 1e-9) / (1024.0 * 1024.0);
    const double decode_mbps = (total_valid * sizeof(float)) / (total_decode_ns * 1e-9) / (1024.0 * 1024.0);

    std::stringstream json;
    json << std::fixed << std::setprecision(6);
    json << "{\n";
    json << "  \"sample_count\": " << n << ",\n";
    json << "  \"total_valid_points\": " << total_valid << ",\n";
    json << "  \"violation_count\": " << total_violations.load() << ",\n";
    json << "  \"max_relative_error\": " << max_rel << ",\n";
    json << "  \"average_total_bytes_per_leaf\": " << avg_total_bytes << ",\n";
    json << "  \"average_record_bytes_per_leaf\": " << avg_record_bytes << ",\n";
    json << "  \"projected_file_ratio\": " << projected_ratio << ",\n";
    json << "  \"raw_fallback_ratio\": " << raw_fallback_ratio << ",\n";
    json << "  \"codec_distribution\": {\n";
    for (int i = 0; i < 6; ++i) {
        json << "    \"" << codecName(static_cast<RzfpLeafCodec>(i)) << "\": "
             << codec_counts[i].load() << (i + 1 == 6 ? "\n" : ",\n");
    }
    json << "  },\n";
    json << "  \"exception_count_distribution\": {\n";
    for (int i = 0; i <= 32; ++i) {
        json << "    \"" << i << "\": " << exc_hist[i].load()
             << (i == 32 ? "\n" : ",\n");
    }
    json << "  },\n";
    json << "  \"size_percentiles_bytes\": {\n";
    json << "    \"P50\": " << pct(0.50) << ",\n";
    json << "    \"P90\": " << pct(0.90) << ",\n";
    json << "    \"P95\": " << pct(0.95) << ",\n";
    json << "    \"P99\": " << pct(0.99) << "\n";
    json << "  },\n";
    json << "  \"encode_MBps\": " << encode_mbps << ",\n";
    json << "  \"decode_MBps\": " << decode_mbps << ",\n";
    json << "  \"elapsed_seconds\": " << elapsed_s << "\n";
    json << "}\n";

    if (!opt.json_path.empty()) {
        std::ofstream jf(opt.json_path);
        jf << json.str();
    }
    std::cout << json.str() << std::endl;

    if (total_violations.load() > 0) {
        std::cerr << "ERROR: " << total_violations.load() << " violations detected" << std::endl;
        return 1;
    }
    return 0;
}
