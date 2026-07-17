#include "erwt3d/rzfp_raw_sampler.hpp"
#include "erwt3d/raw_layout.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace erwt3d {

namespace {

static bool readFullyAt(int fd, void* buffer, size_t bytes, uint64_t offset) {
    auto* dst = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    while (done < bytes) {
        ssize_t n = pread(fd, dst + done, bytes - done, static_cast<off_t>(offset + done));
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return true;
}

} // namespace

bool sampleXPlanesFromRaw(
    int fd,
    uint64_t nx,
    uint64_t ny,
    uint64_t nz,
    const std::vector<uint64_t>& sampled_x,
    const std::vector<RawZRange>& z_ranges,
    std::vector<SampledXPlane>& output
) {
    if (fd < 0 || nx == 0 || ny == 0 || nz == 0) return false;

    std::vector<RawZRange> ranges = z_ranges;
    if (ranges.empty()) {
        ranges.push_back({0, nz});
    }

    for (const auto& zr : ranges) {
        if (zr.z_start + zr.z_count > nz) return false;
    }

    output.clear();
    output.reserve(sampled_x.size() * ranges.size());
    for (uint64_t x : sampled_x) {
        if (x >= nx) return false;
        for (const auto& zr : ranges) {
            output.emplace_back(x, zr.z_start, zr.z_count, ny);
        }
    }

    const uint64_t plane_floats = ny * nz;
    std::vector<float> full_plane(plane_floats);

    for (uint64_t x : sampled_x) {
        const uint64_t offset = x * plane_floats * sizeof(float);
        if (!readFullyAt(fd, full_plane.data(), plane_floats * sizeof(float), offset)) {
            return false;
        }

        for (auto& plane : output) {
            if (plane.x != x) continue;
            for (uint64_t z = plane.z_start; z < plane.z_start + plane.z_count; ++z) {
                float* dst = plane.row(z);
                for (uint64_t y = 0; y < ny; ++y) {
                    dst[y] = full_plane[rawOffsetZFastest(0, y, z, ny, nz)];
                }
            }
        }
    }

    return true;
}

bool sampleXPlanesFromRaw(
    int fd,
    uint64_t nx,
    uint64_t ny,
    uint64_t nz,
    const std::vector<uint64_t>& sampled_x,
    std::vector<SampledXPlane>& output
) {
    return sampleXPlanesFromRaw(fd, nx, ny, nz, sampled_x, {}, output);
}

} // namespace erwt3d
