#include <cstdio>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <random>

static void generate_chunk(uint64_t nx, uint64_t ny, uint64_t nz,
                           uint64_t x_start, uint64_t x_end,
                           uint32_t seed, const char* path) {
    FILE* f = fopen(path, "rb+");
    if (!f) { perror("fopen"); return; }

    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);

    constexpr size_t BUF_SIZE = 1UL << 20;
    std::vector<float> buf(BUF_SIZE);
    size_t buf_idx = 0;

    uint64_t yz = ny * nz;

    for (uint64_t x = x_start; x < x_end; ++x) {
        uint64_t x_offset = x * yz * sizeof(float);
        for (uint64_t y = 0; y < ny; ++y) {
            for (uint64_t z = 0; z < nz; ++z) {
                buf[buf_idx++] = dist(rng);
                if (buf_idx == BUF_SIZE) {
                    fwrite(buf.data(), sizeof(float), BUF_SIZE, f);
                    buf_idx = 0;
                }
            }
        }
    }
    if (buf_idx > 0)
        fwrite(buf.data(), sizeof(float), buf_idx, f);

    fclose(f);
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
    double gb = total * 4.0 / (1024.0*1024.0*1024.0);
    printf("Generating: %lu x %lu x %lu = %lu floats (%.1f GB), %d threads\n",
           nx, ny, nz, total, gb, nthreads);

    // Pre-allocate file
    FILE* f = fopen(path, "wb");
    if (!f) { perror("fopen"); return 1; }
    uint64_t file_bytes = total * sizeof(float);
    if (fseeko(f, file_bytes - 1, SEEK_SET) < 0) { perror("fseeko"); return 1; }
    fputc(0, f);
    fclose(f);

    auto t0 = std::chrono::steady_clock::now();

    // Launch threads
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
    double mb_per_sec = (total * 4.0 / (1024.0*1024.0)) / sec;
    printf("Done: %.1f s, %.0f MB/s\n", sec, mb_per_sec);
    return 0;
}
