#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>
#include <vector>
#include <atomic>
#include <thread>

namespace {

int g_failures = 0;

void check(bool condition, const char* msg) {
    if (!condition) {
        std::cerr << "FAIL: " << msg << std::endl;
        ++g_failures;
    }
}

bool pwriteAll(int fd, const void* buf, size_t bytes, uint64_t offset) {
    const auto* src = static_cast<const uint8_t*>(buf);
    size_t done = 0;
    while (done < bytes) {
        ssize_t n = pwrite(fd, src + done, bytes - done, static_cast<off_t>(offset + done));
        if (n < 0) { if (errno == EINTR) continue; return false; }
        done += static_cast<size_t>(n);
    }
    return true;
}

void generateChunk(uint64_t nx, uint64_t ny, uint64_t nz,
                   uint64_t x_start, uint64_t x_end,
                   const char* path, std::atomic<bool>& failed) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) { failed.store(true); return; }

    std::vector<float> row(nz);

    for (uint64_t x = x_start; x < x_end; ++x) {
        const uint64_t x_offset = x * ny * nz * sizeof(float);
        for (uint64_t y = 0; y < ny; ++y) {
            const uint64_t y_offset = y * nz * sizeof(float);
            for (uint64_t z = 0; z < nz; ++z) {
                row[z] = static_cast<float>(x * 1000000ULL + y * 1000ULL + z);
            }
            if (!pwriteAll(fd, row.data(), nz * sizeof(float), x_offset + y_offset)) {
                failed.store(true);
                close(fd);
                return;
            }
        }
    }
    close(fd);
}

void testParallelWrite(const std::string& path,
                       uint64_t nx, uint64_t ny, uint64_t nz,
                       int nthreads) {
    unlink(path.c_str());

    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "create file");
    uint64_t file_bytes = nx * ny * nz * sizeof(float);
    check(ftruncate(fd, static_cast<off_t>(file_bytes)) == 0, "ftruncate");
    close(fd);

    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    uint64_t chunk = (nx + nthreads - 1) / nthreads;
    for (int i = 0; i < nthreads; ++i) {
        uint64_t xs = i * chunk;
        uint64_t xe = std::min(xs + chunk, nx);
        if (xs >= nx) break;
        threads.emplace_back(generateChunk, nx, ny, nz, xs, xe, path.c_str(), std::ref(failed));
    }
    for (auto& t : threads) t.join();

    check(!failed.load(), "no worker failures");
    check(!threads.empty(), "threads created");

    uint64_t fileSize = 0;
    int fd2 = open(path.c_str(), O_RDONLY); check(fd2 >= 0, "open for size"); fileSize = lseek(fd2, 0, SEEK_END); close(fd2);
    check(fileSize == file_bytes, "file size correct");

    std::vector<float> data(nx * ny * nz);
    fd = open(path.c_str(), O_RDONLY);
    check(fd >= 0, "open for read");
    size_t total = data.size() * sizeof(float);
    size_t done = 0;
    while (done < total) {
        ssize_t n = read(fd, reinterpret_cast<uint8_t*>(data.data()) + done, total - done);
        if (n <= 0) break;
        done += static_cast<size_t>(n);
    }
    close(fd);
    check(done == total, "read full file");

    for (uint64_t x = 0; x < nx; ++x) {
        uint64_t x_base = x * 1000000ULL;
        for (uint64_t y = 0; y < ny; ++y) {
            uint64_t y_base = y * 1000ULL;
            for (uint64_t z = 0; z < nz; ++z) {
                uint64_t idx = (x * ny + y) * nz + z;
                float expected = static_cast<float>(x_base + y_base + z);
                if (data[idx] != expected) {
                    std::cerr << "FAIL: value mismatch at (" << x << "," << y << "," << z
                              << ") expected=" << expected << " got=" << data[idx] << std::endl;
                    ++g_failures;
                    unlink(path.c_str());
                    return;
                }
            }
        }
    }
    unlink(path.c_str());
}

} // namespace

int main() {
    std::vector<int> threadCounts = {1, 4, 8};

    for (int nthreads : threadCounts) {
        std::cout << "=== gen_fast_data " << nthreads << " threads ===" << std::endl;

        testParallelWrite("/tmp/gen_fast_data_test.raw", 16, 8, 4, nthreads);
        testParallelWrite("/tmp/gen_fast_data_test.raw", 5, 7, 11, nthreads);
    }

    if (g_failures == 0) {
        std::cout << "PASS" << std::endl;
        return 0;
    }
    std::cerr << g_failures << " failures" << std::endl;
    return 1;
}
