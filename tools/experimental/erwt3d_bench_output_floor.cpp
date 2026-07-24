#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

uint64_t computeTotalBytes(uint64_t nx, uint64_t ny, uint64_t nz) {
    uint64_t xSlice = ny * nz * sizeof(float);
    uint64_t ySlice = nx * nz * sizeof(float);
    uint64_t zSlice = nx * ny * sizeof(float);
    return 110 * (xSlice + ySlice + zSlice);  // 100 random + 10 cont per axis
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 5) {
        std::cerr << "Usage: erwt3d_bench_output_floor <nx> <ny> <nz> <output-dir> [threads]\n";
        return 1;
    }
    uint64_t nx = std::stoull(argv[1]);
    uint64_t ny = std::stoull(argv[2]);
    uint64_t nz = std::stoull(argv[3]);
    std::string dir = argv[4];
    int threads = argc >= 6 ? std::stoi(argv[5]) : 1;

    uint64_t xb = ny * nz * sizeof(float);
    uint64_t yb = nx * nz * sizeof(float);
    uint64_t zb = nx * ny * sizeof(float);
    uint64_t total = computeTotalBytes(nx, ny, nz);

    uint64_t maxSlice = xb;
    if (yb > maxSlice) maxSlice = yb;
    if (zb > maxSlice) maxSlice = zb;
    std::vector<uint8_t> pattern(maxSlice);
    std::memset(pattern.data(), 0xAB, pattern.size());

    auto t0 = std::chrono::steady_clock::now();

    const char* labels[] = {"x_random", "y_random", "z_random", "x_continuous", "y_continuous", "z_continuous"};
    uint64_t sizes[] = {xb, yb, zb, xb, yb, zb};
    int counts[] = {100, 100, 100, 10, 10, 10};

    size_t fileCount = 0;
    for (int g = 0; g < 6; ++g) {
        for (int i = 0; i < counts[g]; ++i) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s_%03d.raw", dir.c_str(), labels[g], i);
            int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { std::cerr << "open failed: " << path << "\n"; return 1; }
            if (ftruncate(fd, static_cast<off_t>(sizes[g])) != 0) {
                close(fd); return 1;
            }
            ssize_t nw = pwrite(fd, pattern.data(), sizes[g], 0);
            if (nw != static_cast<ssize_t>(sizes[g])) {
                close(fd); return 1;
            }
            close(fd);
            ++fileCount;
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::cout << "Output floor benchmark:\n"
              << "  dims: " << nx << "x" << ny << "x" << nz << "\n"
              << "  files: " << fileCount << "\n"
              << "  total bytes: " << total / 1e6 << " MB (" << total / 1e9 << " GB)\n"
              << "  time: " << ms << " ms (" << ms / 1000.0 << " s)\n"
              << "  throughput: " << (total / 1e6) / (ms / 1000) << " MB/s\n";
    return 0;
}
