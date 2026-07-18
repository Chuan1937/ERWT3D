#include "erwt3d/rzfp_raw_sampler.hpp"

#include <cmath>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* msg) {
    if (!condition) {
        std::cerr << "FAIL: " << msg << std::endl;
        ++g_failures;
    }
}

bool writeZFastestRaw(const std::string& path, uint64_t nx, uint64_t ny, uint64_t nz) {
    std::vector<float> data(nx * ny * nz);
    for (uint64_t x = 0; x < nx; ++x) {
        for (uint64_t y = 0; y < ny; ++y) {
            for (uint64_t z = 0; z < nz; ++z) {
                data[(x * ny + y) * nz + z] = static_cast<float>(x * 1000000 + y * 1000 + z);
            }
        }
    }

    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    size_t done = 0;
    const size_t total = data.size() * sizeof(float);
    while (done < total) {
        ssize_t n = write(fd, reinterpret_cast<const uint8_t*>(data.data()) + done, total - done);
        if (n <= 0) {
            close(fd);
            return false;
        }
        done += static_cast<size_t>(n);
    }
    close(fd);
    return true;
}

} // namespace

int main() {
    const std::string path = "/tmp/erwt3d_test_sampler.raw";
    const uint64_t nx = 25;
    const uint64_t ny = 31;
    const uint64_t nz = 17;

    check(writeZFastestRaw(path, nx, ny, nz), "write raw file");

    int fd = open(path.c_str(), O_RDONLY);
    check(fd >= 0, "open raw file");

    std::vector<uint64_t> xs = {0, nx / 2, nx - 1};
    std::vector<erwt3d::SampledXPlane> planes;
    bool ok = erwt3d::sampleXPlanesFromRaw(fd, nx, ny, nz, xs, planes);
    check(ok, "sampleXPlanesFromRaw returned true");
    check(planes.size() == 3, "correct number of planes");

    for (const auto& plane : planes) {
        check(plane.ny == ny, "plane ny matches");
        check(plane.z_count == nz, "plane z_count matches");
        check(plane.z_start == 0, "plane z_start matches");
        check(plane.data.size() == ny * nz, "plane data size");
        for (uint64_t z = 0; z < nz; ++z) {
            for (uint64_t y = 0; y < ny; ++y) {
                const float expected = static_cast<float>(plane.x * 1000000 + y * 1000 + z);
                const float actual = plane.at(y, z);
                check(actual == expected, "sampled value matches");
            }
        }
    }

    std::vector<erwt3d::RawZRange> ranges = {{0, 5}, {10, 4}};
    std::vector<erwt3d::SampledXPlane> subplanes;
    ok = erwt3d::sampleXPlanesFromRaw(fd, nx, ny, nz, {1ULL}, ranges, subplanes);
    check(ok, "sample with z ranges");
    check(subplanes.size() == 2, "two subplanes");
    check(subplanes[0].z_count == 5, "first range count");
    check(subplanes[1].z_start == 10, "second range start");

    for (const auto& plane : subplanes) {
        for (uint64_t z = plane.z_start; z < plane.z_start + plane.z_count; ++z) {
            for (uint64_t y = 0; y < ny; ++y) {
                const float expected = static_cast<float>(plane.x * 1000000 + y * 1000 + z);
                check(plane.at(y, z) == expected, "subplane value matches");
            }
        }
    }

    close(fd);
    unlink(path.c_str());

    if (g_failures == 0) {
        std::cout << "PASS" << std::endl;
        return 0;
    }
    std::cerr << g_failures << " failures" << std::endl;
    return 1;
}
