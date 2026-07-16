#include <cstdio>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <random>

#include <fcntl.h>
#include <unistd.h>

static bool pwriteAll(int fd, const void* buf, size_t bytes, uint64_t offset) {
    const auto* src = static_cast<const uint8_t*>(buf);
    size_t done = 0;
    while (done < bytes) {
        ssize_t n = pwrite(fd, src + done, bytes - done, static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return true;
}

static void generate_chunk(uint64_t nx, uint64_t ny, uint64_t nz,
                           uint64_t x_start, uint64_t x_end,
                           uint32_t seed, const char* path) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) { perror("open"); return; }

    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);

    constexpr size_t BUF_SIZE = 1UL << 20;
    std::vector<float> buf(BUF_SIZE);
    size_t buf_idx = 0;

    const uint64_t yz = ny * nz;

    for (uint64_t x = x_start; x < x_end; ++x) {
        const uint64_t x_offset = x * yz * sizeof(float);
        for (uint64_t y = 0; y < ny; ++y) {
            const uint64_t y_offset = y * nz * sizeof(float);
            for (uint64_t z = 0; z < nz; ++z) {
                buf[buf_idx++] = dist(rng);
                if (buf_idx == BUF_SIZE) {
                    const uint64_t written = (x_offset + y_offset + (z + 1 - BUF_SIZE) * sizeof(float));
                    if (!pwriteAll(fd, buf.data(), BUF_SIZE * sizeof(float), written)) {
                        perror("pwrite");
                        close(fd);
                        return;
                    }
                    buf_idx = 0;
                }
            }
        }
    }
    if (buf_idx > 0) {
        const uint64_t tail_start = (x_end - 1) * yz * sizeof(float) +
                                    (ny - 1) * nz * sizeof(float) +
                                    (nz - buf_idx) * sizeof(float);
        if (!pwriteAll(fd, buf.data(), buf_idx * sizeof(float), tail_start)) {
            perror("pwrite");
        }
    }

    close(fd);
}

int main(int argc, char** argv) {
    if (argc < 6) {
        fprintf(stderr, "Usage: %s nx ny nz output threads [seed]\n", argv[0]);
        return 1;
    }
    uint64_t nx = std::stoull(argv[1]);
    uint64_t ny = std::stoull(argv[2]);
    uint64_t nz = std::stoull(argv[3]);
    const char* path = argv[4];
    int nthreads = std::stoi(argv[5]);
    uint32_t seed = (argc > 6) ? std::stoul(argv[6]) : 42;

    uint64_t total = nx * ny * nz;
    double gb = total * 4.0 / (1024.0 * 1024.0 * 1024.0);
    printf("Generating: %lu x %lu x %lu = %lu floats (%.1f GB), %d threads\n",
           nx, ny, nz, total, gb, nthreads);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return 1; }
    uint64_t file_bytes = total * sizeof(float);
    if (ftruncate(fd, static_cast<off_t>(file_bytes)) != 0) {
        perror("ftruncate");
        close(fd);
        return 1;
    }
    close(fd);

    auto t0 = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    uint64_t chunk = (nx + nthreads - 1) / nthreads;
    for (int i = 0; i < nthreads; ++i) {
        uint64_t xs = i * chunk;
        uint64_t xe = std::min(xs + chunk, nx);
        if (xs >= nx) break;
        threads.emplace_back(generate_chunk, nx, ny, nz, xs, xe, seed + i, path);
    }
    for (auto& t : threads) t.join();

    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    double mb_per_sec = (total * 4.0 / (1024.0 * 1024.0)) / sec;
    printf("Done: %.1f s, %.0f MB/s\n", sec, mb_per_sec);
    return 0;
}
