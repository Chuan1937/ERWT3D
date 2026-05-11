#pragma once

#include <cstdint>

namespace erwt3d {

// Morton encoding for 3D coordinates
// Each coordinate must fit in 21 bits (max value 2^21 - 1 = 2097151)
uint64_t morton3D(uint32_t x, uint32_t y, uint32_t z);

// Inverse Morton decoding
void unmorton3D(uint64_t code, uint32_t& x, uint32_t& y, uint32_t& z);

// Helper to spread bits for Morton encoding
inline uint64_t spreadBits(uint32_t v) {
    uint64_t x = v;
    x = (x | (x << 32)) & 0x1F00000000FFFF;
    x = (x | (x << 16)) & 0x1F0000FF0000FF;
    x = (x | (x << 8))  & 0x100F00F00F00F00F;
    x = (x | (x << 4))  & 0x10C30C30C30C30C3;
    x = (x | (x << 2))  & 0x1249249249249249;
    return x;
}

// Helper to compact bits for Morton decoding
inline uint32_t compactBits(uint64_t x) {
    x &= 0x1249249249249249;
    x = (x | (x >> 2))  & 0x10C30C30C30C30C3;
    x = (x | (x >> 4))  & 0x100F00F00F00F00F;
    x = (x | (x >> 8))  & 0x1F0000FF0000FF;
    x = (x | (x >> 16)) & 0x1F00000000FFFF;
    x = (x | (x >> 32)) & 0x1FFFFF;
    return static_cast<uint32_t>(x);
}

} // namespace erwt3d