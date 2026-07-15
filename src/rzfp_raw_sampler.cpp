#include "erwt3d/rzfp_raw_sampler.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

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

bool readRawLayer(
    int fd,
    uint64_t nx,
    uint64_t ny,
    uint64_t z,
    std::vector<float>& layer
) {
    layer.resize(nx * ny);
    const uint64_t offset = z * nx * ny * sizeof(float);
    return readFullyAt(fd, layer.data(), layer.size() * sizeof(float), offset);
}

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

    std::vector<float> layer;
    for (const auto& zr : ranges) {
        for (uint64_t z = zr.z_start; z < zr.z_start + zr.z_count; ++z) {
            if (!readRawLayer(fd, nx, ny, z, layer)) return false;
            for (auto& plane : output) {
                if (plane.z_start > z || z >= plane.z_start + plane.z_count) continue;
                float* dst = plane.row(z);
                const uint64_t x = plane.x;
                for (uint64_t y = 0; y < ny; ++y) {
                    dst[y] = layer[y * nx + x];
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
