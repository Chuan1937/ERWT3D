#include <erwt3d/raw_layout.hpp>
#include <erwt3d/raw_x_aux.hpp>
#include <erwt3d/taps_format.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <random>
#include <unordered_map>
#include <unistd.h>
#include <vector>

using namespace erwt3d;

int main(int argc, char* argv[]) {
    std::string raw_path;
    std::string taps_dir;
    uint64_t nx = 0, ny = 0, nz = 0;
    uint64_t samples = 100000;
    uint64_t seed = 42;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--raw" && i + 1 < argc) { raw_path = argv[++i]; }
        else if (arg == "--taps-dir" && i + 1 < argc) { taps_dir = argv[++i]; }
        else if (arg == "--nx" && i + 1 < argc) { nx = std::stoull(argv[++i]); }
        else if (arg == "--ny" && i + 1 < argc) { ny = std::stoull(argv[++i]); }
        else if (arg == "--nz" && i + 1 < argc) { nz = std::stoull(argv[++i]); }
        else if (arg == "--samples" && i + 1 < argc) { samples = std::stoull(argv[++i]); }
        else if (arg == "--seed" && i + 1 < argc) { seed = std::stoull(argv[++i]); }
        else {
            std::fprintf(stderr,
                "Usage: %s --raw data.raw --taps-dir data.taps "
                "--nx N --ny N --nz N [--samples N] [--seed N]\n", argv[0]);
            return 1;
        }
    }

    if (raw_path.empty() || taps_dir.empty() || nx == 0 || ny == 0 || nz == 0) {
        std::fprintf(stderr, "Missing required arguments\n");
        return 1;
    }

    ScopedFd raw_fd(open(raw_path.c_str(), O_RDONLY));
    if (!raw_fd.valid()) { std::fprintf(stderr, "Cannot open raw\n"); return 1; }

    TapsReader reader(taps_dir);

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<uint64_t> dx(0, nx - 1);
    std::uniform_int_distribution<uint64_t> dy(0, ny - 1);
    std::uniform_int_distribution<uint64_t> dz(0, nz - 1);

    struct Sample { uint64_t x, y, z; };
    std::vector<Sample> pts(samples);
    for (auto& p : pts) { p.x = dx(rng); p.y = dy(rng); p.z = dz(rng); }

    std::unordered_map<uint64_t, std::vector<float>> x_cache, y_cache, z_cache;
    auto get_x = [&](uint64_t x) -> float* {
        auto it = x_cache.find(x);
        if (it != x_cache.end()) return it->second.data();
        std::vector<float> buf(ny * nz);
        if (!reader.readSlice('X', x, buf.data())) return nullptr;
        auto& slot = x_cache[x];
        slot = std::move(buf);
        return slot.data();
    };
    auto get_y = [&](uint64_t y) -> float* {
        auto it = y_cache.find(y);
        if (it != y_cache.end()) return it->second.data();
        std::vector<float> buf(nx * nz);
        if (!reader.readSlice('Y', y, buf.data())) return nullptr;
        auto& slot = y_cache[y];
        slot = std::move(buf);
        return slot.data();
    };
    auto get_z = [&](uint64_t z) -> float* {
        auto it = z_cache.find(z);
        if (it != z_cache.end()) return it->second.data();
        std::vector<float> buf(nx * ny);
        if (!reader.readSlice('Z', z, buf.data())) return nullptr;
        auto& slot = z_cache[z];
        slot = std::move(buf);
        return slot.data();
    };

    double max_rel_err = 0;
    uint64_t failures = 0;
    constexpr double rel_tol = 1e-3;

    std::fprintf(stderr, "Verifying %lu random points (batch mode)...\n", samples);
    auto t0 = std::chrono::high_resolution_clock::now();

    for (uint64_t s = 0; s < samples; ++s) {
        auto& p = pts[s];
        uint64_t raw_off = rawOffsetZFastest(p.x, p.y, p.z, ny, nz) * sizeof(float);
        float raw_val;
        readFullyAt(raw_fd.get(), &raw_val, sizeof(float), raw_off);

        float taps_val;
        float* plane = get_x(p.x);
        if (plane) {
            taps_val = plane[p.y * nz + p.z];
        } else {
            plane = get_y(p.y);
            if (plane) {
                taps_val = plane[p.x * nz + p.z];
            } else {
                plane = get_z(p.z);
                taps_val = plane[p.x * ny + p.y];
            }
        }

        if (raw_val != 0.0f) {
            double rel = std::abs(static_cast<double>(taps_val) - static_cast<double>(raw_val))
                       / std::abs(static_cast<double>(raw_val));
            if (rel > max_rel_err) max_rel_err = rel;
            if (rel > rel_tol) failures++;
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();

    std::fprintf(stderr, "Results: %lu samples, max_rel_err=%.6e, failures=%lu, %.1fs\n",
                 samples, max_rel_err, failures, elapsed);
    std::fprintf(stderr, "  X planes cached: %lu, Y: %lu, Z: %lu\n",
                 x_cache.size(), y_cache.size(), z_cache.size());

    if (failures > 0) { std::fprintf(stderr, "FAIL\n"); return 1; }
    std::fprintf(stderr, "PASS\n");
    return 0;
}
