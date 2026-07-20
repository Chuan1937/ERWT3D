#include <erwt3d/raw_layout.hpp>
#include <erwt3d/raw_x_aux.hpp>
#include <erwt3d/taps_format.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <random>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace erwt3d;

static uint64_t total_bytes_written = 0;

static bool writeSliceFile(const std::string& path, const float* data, uint64_t floats) {
    ScopedFd fd(open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644));
    if (!fd.valid()) return false;
    uint64_t bytes = floats * sizeof(float);
    if (!writeFullyAt(fd.get(), data, bytes, 0)) return false;
    total_bytes_written += bytes;
    return true;
}

int main(int argc, char* argv[]) {
    std::string taps_dir;
    std::string output_dir;
    int threads = 8;
    uint64_t seed = 20260511;
    uint64_t random_count = 100;
    uint64_t continuous_count = 10;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--taps-dir" && i + 1 < argc) { taps_dir = argv[++i]; }
        else if (arg == "--output-dir" && i + 1 < argc) { output_dir = argv[++i]; }
        else if (arg == "--threads" && i + 1 < argc) { threads = std::stoi(argv[++i]); }
        else if (arg == "--seed" && i + 1 < argc) { seed = std::stoull(argv[++i]); }
        else if (arg == "--random-count" && i + 1 < argc) { random_count = std::stoull(argv[++i]); }
        else if (arg == "--continuous-count" && i + 1 < argc) { continuous_count = std::stoull(argv[++i]); }
        else {
            std::fprintf(stderr,
                "Usage: %s --taps-dir data.taps --output-dir out "
                "[--threads N] [--seed N] [--random-count N] [--continuous-count N]\n",
                argv[0]);
            return 1;
        }
    }

    if (taps_dir.empty() || output_dir.empty()) {
        std::fprintf(stderr, "Missing required arguments\n");
        return 1;
    }

    TapsReader reader(taps_dir, threads);
    uint64_t nx = reader.nx(), ny = reader.ny(), nz = reader.nz();

    mkdir(output_dir.c_str(), 0755);

    std::mt19937_64 rng(seed);

    struct Group {
        char axis;
        bool random;
        std::vector<uint64_t> indices;
        std::string label;
    };

    std::vector<Group> groups;

    auto addGroup = [&](char axis, bool random, const std::vector<uint64_t>& idx, const std::string& label) {
        groups.push_back({axis, random, idx, label});
    };

    for (char axis : {'X', 'Y', 'Z'}) {
        uint64_t dim = (axis == 'X') ? nx : (axis == 'Y') ? ny : nz;
        std::uniform_int_distribution<uint64_t> dist(0, dim - 1);

        std::vector<uint64_t> rand_idx;
        for (uint64_t i = 0; i < random_count; ++i) rand_idx.push_back(dist(rng));
        std::sort(rand_idx.begin(), rand_idx.end());
        rand_idx.erase(std::unique(rand_idx.begin(), rand_idx.end()), rand_idx.end());
        addGroup(axis, true, rand_idx, std::string(1, axis) + "_random");

        std::uniform_int_distribution<uint64_t> cdist(0, dim - continuous_count);
        uint64_t start = cdist(rng);
        std::vector<uint64_t> cont_idx;
        for (uint64_t i = 0; i < continuous_count; ++i) cont_idx.push_back(start + i);
        addGroup(axis, false, cont_idx, std::string(1, axis) + "_continuous");
    }

    std::fprintf(stderr, "TAPS Contest Benchmark\n");
    std::fprintf(stderr, "  Dims: %lu x %lu x %lu\n", nx, ny, nz);
    std::fprintf(stderr, "  Storage: %.4fx\n", reader.storageRatio());
    std::fprintf(stderr, "  Groups: %lu\n", groups.size());

    std::vector<double> group_times;

    for (size_t gi = 0; gi < groups.size(); ++gi) {
        auto& g = groups[gi];
        uint64_t plane_floats;
        switch (g.axis) {
            case 'X': plane_floats = ny * nz; break;
            case 'Y': plane_floats = nx * nz; break;
            case 'Z': plane_floats = nx * ny; break;
        }

        std::vector<float> bufs(g.indices.size() * plane_floats);
        std::vector<TapsSliceRequest> reqs;
        for (size_t i = 0; i < g.indices.size(); ++i) {
            reqs.push_back({g.axis, g.indices[i], bufs.data() + i * plane_floats});
        }

        auto t0 = std::chrono::high_resolution_clock::now();

        if (!reader.readSlicesBatch(reqs)) {
            std::fprintf(stderr, "Batch read failed for %s\n", g.label.c_str());
            return 1;
        }

        for (size_t i = 0; i < g.indices.size(); ++i) {
            char fname[256];
            std::snprintf(fname, sizeof(fname), "%s/%c_%lu.raw",
                          output_dir.c_str(), g.axis, g.indices[i]);
            if (!writeSliceFile(fname, bufs.data() + i * plane_floats, plane_floats)) {
                std::fprintf(stderr, "Write failed: %s\n", fname);
                return 1;
            }
        }

        auto t1 = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double>(t1 - t0).count();
        group_times.push_back(elapsed);

        std::fprintf(stderr, "  [%lu/%lu] %s: %.4fs (%lu slices)\n",
                     gi + 1, groups.size(), g.label.c_str(), elapsed, g.indices.size());
    }

    double t_composite = 0;
    for (auto t : group_times) t_composite += t;
    t_composite /= groups.size();

    std::fprintf(stderr, "\n=== Results ===\n");
    std::fprintf(stderr, "T_composite: %.4fs\n", t_composite);
    std::fprintf(stderr, "Output bytes: %lu\n", total_bytes_written);

    for (size_t i = 0; i < groups.size(); ++i) {
        std::fprintf(stderr, "  %s: %.4fs\n", groups[i].label.c_str(), group_times[i]);
    }

    return 0;
}
