#pragma once

#include <cstdint>

namespace erwt3d {

// Official input data layout (Z-fastest):
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

inline uint64_t rawOffsetBytesZFastest(
    uint64_t x,
    uint64_t y,
    uint64_t z,
    uint64_t ny,
    uint64_t nz
) {
    return rawOffsetZFastest(x, y, z, ny, nz) * sizeof(float);
}

// Offset of a complete X-plane (YZ slice) in the official raw layout.
// A full X-plane is contiguous in the input file.
inline uint64_t rawXPlaneOffset(
    uint64_t x,
    uint64_t ny,
    uint64_t nz
) {
    return x * ny * nz;
}

inline uint64_t rawXPlaneBytes(
    uint64_t ny,
    uint64_t nz
) {
    return ny * nz * sizeof(float);
}

// Legacy/internal helper: X-fastest layout used only for internal
// scratch buffers or old test data. NEVER use this for official raw input.
inline uint64_t rawOffsetXFastest(
    uint64_t x,
    uint64_t y,
    uint64_t z,
    uint64_t nx,
    uint64_t ny
) {
    return (z * ny + y) * nx + x;
}

// Blocked transpose: Z-major (zi*ny+y) → Z-fastest output (y*nz+z).
// Used by sidecar/legacy X-plane reader paths after decompression.
// Blocks improve cache locality for large planes (~24MB per X-plane).
inline void transposeZYToYZ(
    const float* __restrict src,
    float* __restrict dst,
    uint64_t srcRows,
    uint64_t ny,
    uint64_t nz
) {
    constexpr uint64_t BY = 32;
    constexpr uint64_t BZ = 32;
    for (uint64_t z0 = 0; z0 < srcRows; z0 += BZ) {
        const uint64_t zEnd = z0 + BZ < srcRows ? z0 + BZ : srcRows;
        for (uint64_t y0 = 0; y0 < ny; y0 += BY) {
            const uint64_t yEnd = y0 + BY < ny ? y0 + BY : ny;
            for (uint64_t y = y0; y < yEnd; ++y) {
                for (uint64_t zi = z0; zi < zEnd; ++zi) {
                    dst[y * nz + zi] = src[zi * ny + y];
                }
            }
        }
    }
}

} // namespace erwt3d
