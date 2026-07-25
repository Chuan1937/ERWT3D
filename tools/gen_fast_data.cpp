#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

enum class DataPattern {
    Random,
    Lz4
};

bool pwriteAll(int fd, const void* buf, size_t bytes, uint64_t offset) {
    const auto* src = static_cast<const uint8_t*>(buf);
    size_t done = 0;
    while (done < bytes) {
        const ssize_t n = pwrite(
            fd,
            src + done,
            bytes - done,
            static_cast<off_t>(offset + done));
        if (n == 0) {
            errno = EIO;
            return false;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return true;
}

float lz4Value(uint64_t x, uint64_t y, uint64_t z, uint32_t seed) {
    // Smooth, low-cardinality 3D blocks with occasional zero regions.  This
    // resembles piecewise scientific volumes and is deliberately compressible
    // without becoming a trivial all-zero correctness fixture.
    const uint64_t bx = x / 8;
    const uint64_t by = y / 8;
    const uint64_t bz = z / 32;
    const uint64_t code =
        (bx * 13 + by * 7 + bz * 3 + seed) & 1023ULL;
    if ((bx + 3 * by + 5 * bz + seed) % 97 == 0) return 0.0f;
    return static_cast<float>(code) * (1.0f / 32.0f);
}

void generateChunk(
    uint64_t ny,
    uint64_t nz,
    uint64_t xStart,
    uint64_t xEnd,
    uint32_t seed,
    DataPattern pattern,
    const char* path,
    std::atomic<bool>& failed
) {
    const int fd = open(path, O_WRONLY);
    if (fd < 0) {
        failed.store(true, std::memory_order_relaxed);
        return;
    }

    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    std::vector<float> row(nz);

    for (uint64_t x = xStart; x < xEnd; ++x) {
        const uint64_t xOffset = x * ny * nz * sizeof(float);
        for (uint64_t y = 0; y < ny; ++y) {
            const uint64_t yOffset = y * nz * sizeof(float);
            for (uint64_t z = 0; z < nz; ++z) {
                row[z] =
                    pattern == DataPattern::Lz4
                        ? lz4Value(x, y, z, seed)
                        : dist(rng);
            }
            if (!pwriteAll(
                    fd,
                    row.data(),
                    nz * sizeof(float),
                    xOffset + yOffset)) {
                failed.store(true, std::memory_order_relaxed);
                close(fd);
                return;
            }
        }
    }
    close(fd);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(
            stderr,
            "Usage: %s nx ny nz output threads [seed] [random|lz4]\n",
            argv[0]);
        return 1;
    }

    const uint64_t nx = std::stoull(argv[1]);
    const uint64_t ny = std::stoull(argv[2]);
    const uint64_t nz = std::stoull(argv[3]);
    const char* path = argv[4];
    const int nthreads = std::stoi(argv[5]);
    const uint32_t seed =
        argc > 6 ? static_cast<uint32_t>(std::stoul(argv[6])) : 42;
    const std::string patternName = argc > 7 ? argv[7] : "random";
    const DataPattern pattern =
        patternName == "lz4"
            ? DataPattern::Lz4
            : DataPattern::Random;

    if (patternName != "random" && patternName != "lz4") {
        std::fprintf(stderr, "pattern must be 'random' or 'lz4'\n");
        return 1;
    }
    if (nthreads <= 0 || nx == 0 || ny == 0 || nz == 0) {
        std::fprintf(
            stderr,
            "threads and dimensions must be positive\n");
        return 1;
    }

    const auto checkedMul =
        [](uint64_t a, uint64_t b, uint64_t& out) {
            if (a != 0 &&
                b > std::numeric_limits<uint64_t>::max() / a) {
                return false;
            }
            out = a * b;
            return true;
        };

    uint64_t total = 0;
    uint64_t fileBytes = 0;
    if (!checkedMul(nx, ny, total) ||
        !checkedMul(total, nz, total) ||
        !checkedMul(total, sizeof(float), fileBytes)) {
        std::fprintf(stderr, "dimensions overflow\n");
        return 1;
    }

    std::printf(
        "Generating: %lu x %lu x %lu = %lu floats (%.1f GiB), "
        "%d threads, pattern=%s\n",
        nx,
        ny,
        nz,
        total,
        static_cast<double>(fileBytes) /
            (1024.0 * 1024.0 * 1024.0),
        nthreads,
        patternName.c_str());

    const int fd = open(
        path,
        O_WRONLY | O_CREAT | O_TRUNC,
        0644);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    if (ftruncate(fd, static_cast<off_t>(fileBytes)) != 0) {
        perror("ftruncate");
        close(fd);
        return 1;
    }
    close(fd);

    const auto start = std::chrono::steady_clock::now();
    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    const uint64_t chunk =
        nx / static_cast<uint64_t>(nthreads) +
        (nx % static_cast<uint64_t>(nthreads) != 0);
    for (int i = 0; i < nthreads; ++i) {
        const uint64_t xStart = static_cast<uint64_t>(i) * chunk;
        const uint64_t xEnd = std::min(xStart + chunk, nx);
        if (xStart >= nx) break;
        workers.emplace_back(
            generateChunk,
            ny,
            nz,
            xStart,
            xEnd,
            pattern == DataPattern::Lz4
                ? seed
                : seed + static_cast<uint32_t>(i),
            pattern,
            path,
            std::ref(failed));
    }
    for (auto& worker : workers) worker.join();

    if (failed.load(std::memory_order_relaxed)) {
        std::fprintf(stderr, "Generation failed\n");
        unlink(path);
        return 1;
    }

    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    const double mibPerSecond =
        static_cast<double>(fileBytes) /
        (1024.0 * 1024.0) /
        seconds;
    std::printf(
        "Done: %.1f s, %.0f MiB/s\n",
        seconds,
        mibPerSecond);
    return 0;
}
