#include <erwt3d/raw_layout.hpp>
#include <erwt3d/raw_x_aux.hpp>
#include <erwt3d/relative_error.hpp>
#include <erwt3d/rzfp_codec.hpp>
#include <erwt3d/rzfp_xplane_codec.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <lz4.h>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

using namespace erwt3d;

static void printUsage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s --input data.raw --nx N --ny N --nz N [options]\n"
        "\n"
        "Probe X/Y/Z plane compression ratios for SSD plane-stream design.\n"
        "\n"
        "Options:\n"
        "  --input PATH       Raw float32 file (required)\n"
        "  --nx N             X dimension (required)\n"
        "  --ny N             Y dimension (required)\n"
        "  --nz N             Z dimension (required)\n"
        "  --samples N        Number of planes to sample per axis (default: 64)\n"
        "  --chunk-kb N       Chunk size in KiB for LZ4 (default: 1024)\n"
        "  --threads N        Threads (default: 1)\n"
        "  --seed N           Random seed (default: 42)\n"
        "  --csv PATH         Write per-plane CSV output\n"
        "  --axis-filter X|Y|Z|ALL  Which axis to probe (default: ALL)\n",
        prog);
}

enum class AxisFilter { All, X, Y, Z };

struct PlaneSample {
    uint64_t plane_index;
    uint64_t raw_bytes;
    uint64_t lz4_bytes;
    double lz4_ratio;
    uint64_t rzfp2d_bytes;
    double rzfp2d_ratio;
    double rzfp2d_max_rel_error;
    uint32_t rzfp2d_violations;
    double encode_ms;
    double decode_ms;
};

struct AxisResult {
    char axis;
    uint64_t plane_count;
    uint64_t raw_bytes_per_plane;
    double avg_lz4_ratio;
    double avg_rzfp2d_ratio;
    double max_rzfp2d_rel_error;
    uint32_t total_rzfp2d_violations;
    double estimated_total_lz4_ratio;
    double estimated_total_rzfp2d_ratio;
    double avg_encode_ms;
    double avg_decode_ms;
    std::vector<PlaneSample> samples;
};

static std::vector<uint64_t> generatePlaneIndices(
    uint64_t dim, uint64_t samples, std::mt19937_64& rng
) {
    std::vector<uint64_t> indices;

    indices.push_back(0);
    if (dim > 1) indices.push_back(dim - 1);
    if (dim > 2) indices.push_back(dim / 2);

    std::set<uint64_t> unique(indices.begin(), indices.end());

    if (samples > unique.size()) {
        std::uniform_int_distribution<uint64_t> dist(0, dim - 1);
        while (unique.size() < samples) {
            unique.insert(dist(rng));
        }
    }

    indices.assign(unique.begin(), unique.end());
    std::sort(indices.begin(), indices.end());
    return indices;
}

static bool readXPlane(
    int fd, uint64_t x, uint64_t ny, uint64_t nz,
    float* buffer
) {
    uint64_t offset = rawXPlaneBytes(ny, nz) * x;
    uint64_t bytes = rawXPlaneBytes(ny, nz);
    return readFullyAt(fd, buffer, bytes, offset);
}

static bool readYPlane(
    int fd, uint64_t y, uint64_t nx, uint64_t ny, uint64_t nz,
    float* buffer
) {
    uint64_t plane_floats = nx * nz;
    for (uint64_t x = 0; x < nx; ++x) {
        uint64_t row_offset = rawOffsetBytesZFastest(x, y, 0, ny, nz);
        if (!readFullyAt(fd, buffer + x * nz, nz * sizeof(float), row_offset))
            return false;
    }
    return true;
}

static bool readZPlane(
    int fd, uint64_t z, uint64_t nx, uint64_t ny, uint64_t nz,
    float* buffer
) {
    for (uint64_t x = 0; x < nx; ++x) {
        for (uint64_t y = 0; y < ny; ++y) {
            uint64_t offset = rawOffsetBytesZFastest(x, y, z, ny, nz);
            if (!readFullyAt(fd, buffer + (x * ny + y), sizeof(float), offset))
                return false;
        }
    }
    return true;
}

static void rearrangeXPlaneToYFastest(
    const float* raw, float* out, uint64_t ny, uint64_t nz
) {
    for (uint64_t y = 0; y < ny; ++y)
        for (uint64_t z = 0; z < nz; ++z)
            out[z * ny + y] = raw[y * nz + z];
}

static void rearrangeYPlaneToContiguous(
    const float* raw, float* out, uint64_t nx, uint64_t nz
) {
    for (uint64_t x = 0; x < nx; ++x)
        for (uint64_t z = 0; z < nz; ++z)
            out[z * nx + x] = raw[x * nz + z];
}

static void rearrangeZPlaneToContiguous(
    const float* raw, float* out, uint64_t nx, uint64_t ny
) {
    for (uint64_t x = 0; x < nx; ++x)
        for (uint64_t y = 0; y < ny; ++y)
            out[y * nx + x] = raw[x * ny + y];
}

static uint64_t lz4CompressPlane(
    const float* plane, uint64_t floats,
    uint64_t chunk_floats, std::vector<uint8_t>& tmp
) {
    uint64_t total_compressed = 0;
    tmp.resize(LZ4_compressBound(static_cast<int>(chunk_floats * sizeof(float))));

    for (uint64_t off = 0; off < floats; off += chunk_floats) {
        uint64_t n = std::min(chunk_floats, floats - off);
        int srcSize = static_cast<int>(n * sizeof(float));
        int dstCapacity = LZ4_compressBound(srcSize);
        int compressed = LZ4_compress_default(
            reinterpret_cast<const char*>(plane + off),
            reinterpret_cast<char*>(tmp.data()),
            srcSize, dstCapacity
        );
        total_compressed += (compressed > 0) ? static_cast<uint64_t>(compressed)
                                              : static_cast<uint64_t>(srcSize);
    }
    return total_compressed;
}

static void probeAxis(
    int fd, char axis, uint64_t nx, uint64_t ny, uint64_t nz,
    uint64_t num_samples, uint64_t chunk_kb,
    std::mt19937_64& rng, AxisResult& result
) {
    uint64_t plane_floats = 0;
    uint64_t dim = 0;

    switch (axis) {
        case 'X': plane_floats = ny * nz; dim = nx; break;
        case 'Y': plane_floats = nx * nz; dim = ny; break;
        case 'Z': plane_floats = nx * ny; dim = nz; break;
    }

    uint64_t raw_plane_bytes = plane_floats * sizeof(float);
    uint64_t chunk_floats = (chunk_kb * 1024) / sizeof(float);
    if (chunk_floats == 0) chunk_floats = plane_floats;

    auto indices = generatePlaneIndices(dim, num_samples, rng);

    result.axis = axis;
    result.plane_count = dim;
    result.raw_bytes_per_plane = raw_plane_bytes;
    result.samples.clear();

    std::vector<float> raw_plane(plane_floats);
    std::vector<float> rearranged(plane_floats);
    std::vector<float> decoded(plane_floats);
    std::vector<uint8_t> lz4_tmp;

    RzfpXPlaneCodecConfig rzfp_config;
    rzfp_config.error.contest_bound = 1e-3;
    rzfp_config.error.internal_bound = 7.5e-4;

    double sum_lz4_ratio = 0;
    double sum_rzfp2d_ratio = 0;
    double sum_encode_ms = 0;
    double sum_decode_ms = 0;
    double max_rel_err = 0;
    uint32_t total_violations = 0;

    std::fprintf(stderr, "Probing axis %c: %lu planes, sampling %lu, "
                 "plane_bytes=%.2f MiB, chunk=%.lu KiB\n",
                 axis, dim, indices.size(),
                 raw_plane_bytes / (1024.0 * 1024.0),
                 chunk_kb);

    for (uint64_t idx : indices) {
        bool ok = true;
        switch (axis) {
            case 'X': ok = readXPlane(fd, idx, ny, nz, raw_plane.data()); break;
            case 'Y': ok = readYPlane(fd, idx, nx, ny, nz, raw_plane.data()); break;
            case 'Z': ok = readZPlane(fd, idx, nx, ny, nz, raw_plane.data()); break;
        }
        if (!ok) {
            std::fprintf(stderr, "  Failed to read %c-plane %lu\n", axis, idx);
            continue;
        }

        switch (axis) {
            case 'X': rearrangeXPlaneToYFastest(raw_plane.data(), rearranged.data(), ny, nz); break;
            case 'Y': rearrangeYPlaneToContiguous(raw_plane.data(), rearranged.data(), nx, nz); break;
            case 'Z': rearrangeZPlaneToContiguous(raw_plane.data(), rearranged.data(), nx, ny); break;
        }

        PlaneSample s;
        s.plane_index = idx;
        s.raw_bytes = raw_plane_bytes;

        auto t0 = std::chrono::high_resolution_clock::now();
        s.lz4_bytes = lz4CompressPlane(rearranged.data(), plane_floats, chunk_floats, lz4_tmp);
        auto t1 = std::chrono::high_resolution_clock::now();
        s.lz4_ratio = static_cast<double>(s.lz4_bytes) / raw_plane_bytes;

        auto t2 = std::chrono::high_resolution_clock::now();
        auto encoded = encodeXPlane2D(rearranged.data(),
            (axis == 'X') ? ny : nx,
            (axis == 'X') ? nz : ((axis == 'Y') ? nz : ny),
            rzfp_config);
        auto t3 = std::chrono::high_resolution_clock::now();

        s.rzfp2d_bytes = encoded.size();
        s.rzfp2d_ratio = static_cast<double>(s.rzfp2d_bytes) / raw_plane_bytes;

        auto t4 = std::chrono::high_resolution_clock::now();
        bool dec_ok = decodeXPlane2D(encoded.data(), encoded.size(),
            decoded.data(),
            (axis == 'X') ? ny : nx,
            (axis == 'X') ? nz : ((axis == 'Y') ? nz : ny));
        auto t5 = std::chrono::high_resolution_clock::now();

        s.rzfp2d_max_rel_error = 0;
        s.rzfp2d_violations = 0;
        if (dec_ok) {
            RelativeErrorConfig err_cfg;
            err_cfg.contest_bound = 1e-3;
            for (uint64_t i = 0; i < plane_floats; ++i) {
                auto r = checkPointwiseError(rearranged[i], decoded[i], err_cfg);
                if (r.relative_error > s.rzfp2d_max_rel_error)
                    s.rzfp2d_max_rel_error = r.relative_error;
                if (!r.passed) s.rzfp2d_violations++;
            }
        }

        s.encode_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
        s.decode_ms = std::chrono::duration<double, std::milli>(t5 - t4).count();

        sum_lz4_ratio += s.lz4_ratio;
        sum_rzfp2d_ratio += s.rzfp2d_ratio;
        sum_encode_ms += s.encode_ms;
        sum_decode_ms += s.decode_ms;
        if (s.rzfp2d_max_rel_error > max_rel_err)
            max_rel_err = s.rzfp2d_max_rel_error;
        total_violations += s.rzfp2d_violations;

        result.samples.push_back(s);

        std::fprintf(stderr, "  %c[%lu] LZ4=%.4fx RZFP2D=%.4fx err=%.6f v=%u "
                     "enc=%.1fms dec=%.1fms\n",
                     axis, idx, s.lz4_ratio, s.rzfp2d_ratio,
                     s.rzfp2d_max_rel_error, s.rzfp2d_violations,
                     s.encode_ms, s.decode_ms);
    }

    double n = static_cast<double>(result.samples.size());
    result.avg_lz4_ratio = sum_lz4_ratio / n;
    result.avg_rzfp2d_ratio = sum_rzfp2d_ratio / n;
    result.max_rzfp2d_rel_error = max_rel_err;
    result.total_rzfp2d_violations = total_violations;
    result.avg_encode_ms = sum_encode_ms / n;
    result.avg_decode_ms = sum_decode_ms / n;

    uint64_t total_raw = static_cast<uint64_t>(nx) * ny * nz * sizeof(float);
    uint64_t idx_overhead = dim * 32;
    uint64_t align_pad = dim * 4096;

    result.estimated_total_lz4_ratio =
        (result.avg_lz4_ratio * total_raw + idx_overhead + align_pad) / total_raw;
    result.estimated_total_rzfp2d_ratio =
        (result.avg_rzfp2d_ratio * total_raw + idx_overhead + align_pad) / total_raw;
}

int main(int argc, char* argv[]) {
    std::string input_path;
    uint64_t nx = 0, ny = 0, nz = 0;
    uint64_t num_samples = 64;
    uint64_t chunk_kb = 1024;
    int threads = 1;
    uint64_t seed = 42;
    std::string csv_path;
    AxisFilter axis_filter = AxisFilter::All;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--input" && i + 1 < argc) { input_path = argv[++i]; }
        else if (arg == "--nx" && i + 1 < argc) { nx = std::stoull(argv[++i]); }
        else if (arg == "--ny" && i + 1 < argc) { ny = std::stoull(argv[++i]); }
        else if (arg == "--nz" && i + 1 < argc) { nz = std::stoull(argv[++i]); }
        else if (arg == "--samples" && i + 1 < argc) { num_samples = std::stoull(argv[++i]); }
        else if (arg == "--chunk-kb" && i + 1 < argc) { chunk_kb = std::stoull(argv[++i]); }
        else if (arg == "--threads" && i + 1 < argc) { threads = std::stoi(argv[++i]); }
        else if (arg == "--seed" && i + 1 < argc) { seed = std::stoull(argv[++i]); }
        else if (arg == "--csv" && i + 1 < argc) { csv_path = argv[++i]; }
        else if (arg == "--axis-filter" && i + 1 < argc) {
            std::string v = argv[++i];
            if (v == "X") axis_filter = AxisFilter::X;
            else if (v == "Y") axis_filter = AxisFilter::Y;
            else if (v == "Z") axis_filter = AxisFilter::Z;
            else axis_filter = AxisFilter::All;
        }
        else {
            printUsage(argv[0]);
            return 1;
        }
    }

    if (input_path.empty() || nx == 0 || ny == 0 || nz == 0) {
        printUsage(argv[0]);
        return 1;
    }

    ScopedFd fd(open(input_path.c_str(), O_RDONLY));
    if (!fd.valid()) {
        std::fprintf(stderr, "Cannot open %s\n", input_path.c_str());
        return 1;
    }

    std::mt19937_64 rng(seed);

    uint64_t total_raw = static_cast<uint64_t>(nx) * ny * nz * sizeof(float);
    std::fprintf(stderr, "Dimensions: %lu x %lu x %lu = %.2f GiB\n",
                 nx, ny, nz, total_raw / (1024.0 * 1024.0 * 1024.0));

    std::vector<AxisResult> results;

    if (axis_filter == AxisFilter::All || axis_filter == AxisFilter::X) {
        AxisResult r;
        probeAxis(fd.get(), 'X', nx, ny, nz, num_samples, chunk_kb, rng, r);
        results.push_back(std::move(r));
    }
    if (axis_filter == AxisFilter::All || axis_filter == AxisFilter::Y) {
        AxisResult r;
        probeAxis(fd.get(), 'Y', nx, ny, nz, num_samples, chunk_kb, rng, r);
        results.push_back(std::move(r));
    }
    if (axis_filter == AxisFilter::All || axis_filter == AxisFilter::Z) {
        AxisResult r;
        probeAxis(fd.get(), 'Z', nx, ny, nz, num_samples, chunk_kb, rng, r);
        results.push_back(std::move(r));
    }

    std::fprintf(stderr, "\n=== Plane Stream Storage Prediction ===\n");
    std::fprintf(stderr, "Raw data: %.2f GiB\n\n", total_raw / (1024.0 * 1024.0 * 1024.0));

    double total_lz4 = 0;
    double total_rzfp2d = 0;

    for (const auto& r : results) {
        std::fprintf(stderr, "Axis %c (%lu planes):\n", r.axis, r.plane_count);
        std::fprintf(stderr, "  LZ4:    avg_ratio=%.4fx  est_total=%.4fx\n",
                     r.avg_lz4_ratio, r.estimated_total_lz4_ratio);
        std::fprintf(stderr, "  RZFP2D: avg_ratio=%.4fx  est_total=%.4fx  "
                     "max_err=%.6f  violations=%u\n",
                     r.avg_rzfp2d_ratio, r.estimated_total_rzfp2d_ratio,
                     r.max_rzfp2d_rel_error, r.total_rzfp2d_violations);
        std::fprintf(stderr, "  Encode: %.1f ms/plane  Decode: %.1f ms/plane\n",
                     r.avg_encode_ms, r.avg_decode_ms);

        total_lz4 += r.avg_lz4_ratio;
        total_rzfp2d += r.avg_rzfp2d_ratio;
    }

    double r_total_lz4 = total_lz4;
    double r_total_rzfp2d = total_rzfp2d;

    std::fprintf(stderr, "\nThree-axis total prediction:\n");
    std::fprintf(stderr, "  3x LZ4:    R_total = %.3fx  %s\n",
                 r_total_lz4,
                 r_total_lz4 <= 1.45 ? "OK (<=1.45)" :
                 r_total_lz4 <= 1.50 ? "WARN (<=1.50)" : "OVER");
    std::fprintf(stderr, "  3x RZFP2D: R_total = %.3fx  %s\n",
                 r_total_rzfp2d,
                 r_total_rzfp2d <= 1.45 ? "OK (<=1.45)" :
                 r_total_rzfp2d <= 1.50 ? "WARN (<=1.50)" : "OVER");

    if (r_total_lz4 <= 1.45) {
        std::fprintf(stderr, "\nRECOMMENDATION: Three-axis LZ4 Plane Stream (space OK)\n");
    } else if (r_total_rzfp2d <= 1.45) {
        std::fprintf(stderr, "\nRECOMMENDATION: Three-axis RZFP2D Plane Stream (LZ4 too large)\n");
    } else if (r_total_lz4 <= 1.50) {
        std::fprintf(stderr, "\nRECOMMENDATION: Mixed LZ4/RZFP2D needed (tight on space)\n");
    } else {
        std::fprintf(stderr, "\nRECOMMENDATION: Primary format + sidecar approach (3-axis exceeds budget)\n");
    }

    if (!csv_path.empty()) {
        std::ofstream csv(csv_path);
        csv << "axis,plane_index,raw_bytes,lz4_bytes,lz4_ratio,"
            << "rzfp2d_bytes,rzfp2d_ratio,rzfp2d_max_rel_error,"
            << "rzfp2d_violations,encode_ms,decode_ms\n";
        for (const auto& r : results) {
            for (const auto& s : r.samples) {
                csv << r.axis << "," << s.plane_index << ","
                    << s.raw_bytes << "," << s.lz4_bytes << ","
                    << std::fixed << std::setprecision(6) << s.lz4_ratio << ","
                    << s.rzfp2d_bytes << "," << s.rzfp2d_ratio << ","
                    << s.rzfp2d_max_rel_error << ","
                    << s.rzfp2d_violations << ","
                    << s.encode_ms << "," << s.decode_ms << "\n";
            }
        }
        std::fprintf(stderr, "CSV written to %s\n", csv_path.c_str());
    }

    return 0;
}
