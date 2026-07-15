#pragma once

#include <cstdint>
#include <vector>

namespace erwt3d {

struct RawZRange {
    uint64_t z_start = 0;
    uint64_t z_count = 0;
};

struct SampledXPlane {
    uint64_t x = 0;
    uint64_t z_start = 0;
    uint64_t z_count = 0;
    uint64_t ny = 0;
    std::vector<float> data;

    SampledXPlane() = default;
    SampledXPlane(uint64_t x_, uint64_t zs, uint64_t zc, uint64_t ny_)
        : x(x_), z_start(zs), z_count(zc), ny(ny_), data(zc * ny_) {}

    float at(uint64_t y, uint64_t z) const {
        return data[(z - z_start) * ny + y];
    }

    float* row(uint64_t z) { return data.data() + (z - z_start) * ny; }
    const float* row(uint64_t z) const { return data.data() + (z - z_start) * ny; }
};

bool sampleXPlanesFromRaw(
    int fd,
    uint64_t nx,
    uint64_t ny,
    uint64_t nz,
    const std::vector<uint64_t>& sampled_x,
    const std::vector<RawZRange>& z_ranges,
    std::vector<SampledXPlane>& output
);

bool sampleXPlanesFromRaw(
    int fd,
    uint64_t nx,
    uint64_t ny,
    uint64_t nz,
    const std::vector<uint64_t>& sampled_x,
    std::vector<SampledXPlane>& output
);

bool readRawLayer(
    int fd,
    uint64_t nx,
    uint64_t ny,
    uint64_t z,
    std::vector<float>& layer
);

} // namespace erwt3d
