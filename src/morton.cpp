#include "erwt3d/morton.hpp"

namespace erwt3d {

uint64_t morton3D(uint32_t x, uint32_t y, uint32_t z) {
    return spreadBits(x) | (spreadBits(y) << 1) | (spreadBits(z) << 2);
}

void unmorton3D(uint64_t code, uint32_t& x, uint32_t& y, uint32_t& z) {
    x = compactBits(code);
    y = compactBits(code >> 1);
    z = compactBits(code >> 2);
}

} // namespace erwt3d