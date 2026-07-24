#include "erwt3d/raw_layout.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <random>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

double nowMs() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char* argv[]) {
    if (argc < 5) {
        std::cerr << "Usage: erwt3d_bench_xplane_raw <raw_file> <nx> <ny> <nz> [count]\n";
        return 1;
    }
    const char* path = argv[1];
    uint64_t nx = std::stoull(argv[2]);
    uint64_t ny = std::stoull(argv[3]);
    uint64_t nz = std::stoull(argv[4]);
    int count = argc >= 6 ? std::stoi(argv[5]) : 100;

    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }

    uint64_t planeBytes = ny * nz * sizeof(float);
    std::vector<float> buf(ny * nz);

    std::mt19937 rng(42);
    std::uniform_int_distribution<uint64_t> dist(0, nx - 1);

    // Random X-plane read
    std::vector<uint64_t> xs(count);
    for (int i = 0; i < count; ++i) xs[i] = dist(rng);
    std::sort(xs.begin(), xs.end());

    double t0 = nowMs();
    for (auto x : xs) {
        uint64_t off = erwt3d::rawXPlaneOffset(x, ny, nz) * sizeof(float);
        ssize_t n = pread(fd, buf.data(), planeBytes, static_cast<off_t>(off));
        if (n != static_cast<ssize_t>(planeBytes)) { close(fd); return 1; }
    }
    double tRand = nowMs() - t0;

    // Continuous X-plane read
    uint64_t xStart = nx / 3;
    t0 = nowMs();
    for (uint64_t x = xStart; x < xStart + 10; ++x) {
        uint64_t off = erwt3d::rawXPlaneOffset(x, ny, nz) * sizeof(float);
        ssize_t n = pread(fd, buf.data(), planeBytes, static_cast<off_t>(off));
        if (n != static_cast<ssize_t>(planeBytes)) { close(fd); return 1; }
    }
    double tCont = nowMs() - t0;

    close(fd);

    printf("AxisPack diagnostic (raw X-plane sequential read):\n");
    printf("  dims: %llux%llux%llu\n", (unsigned long long)nx, (unsigned long long)ny, (unsigned long long)nz);
    printf("  plane bytes: %.1f MB\n", planeBytes / 1e6);
    printf("  %d random X-planes: %.1f ms (%.1f MB/s)\n",
           count, tRand, (count * planeBytes / 1e6) / (tRand / 1000));
    printf("  10 continuous X-planes: %.1f ms (%.1f MB/s)\n",
           tCont, (10 * planeBytes / 1e6) / (tCont / 1000));
    return 0;
}
