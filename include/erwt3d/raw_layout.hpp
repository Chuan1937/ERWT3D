#pragma once

#include <cstdint>

namespace erwt3d {

// Official data layout (Z-fastest):
// data is stored in X-Y-Z order, meaning x is the slowest varying index
// and z is the fastest varying index.
// offset(x, y, z) = (x * ny + y) * nz + z

inline uint64_t rawOffsetZFastest(
    uint64_t x,
    uint64_t y,
    uint64_t z,
    uint64_t ny,
    uint64_t nz
) {
    return (x * ny + y) * nz + z;
}

inline uint64_t rawOffsetXFastest(
    uint64_t x,
    uint64_t y,
    uint64_t z,
    uint64_t nx,
    uint64_t ny
) {
    return (z * ny + y) * nx + x;
}

} // namespace erwt3d
