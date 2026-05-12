#include "erwt3d/writer.hpp"
#include "erwt3d/reader.hpp"
#include <iostream>
#include <vector>
#include <cmath>

using namespace erwt3d;

static int gFail = 0;

static float ref(uint64_t x, uint64_t y, uint64_t z) {
    return static_cast<float>(x + 1000*y + 1000000*z);
}

static void testEquiv(uint64_t nx, uint64_t ny, uint64_t nz, const char* label) {
    uint64_t n = nx * ny * nz;
    std::vector<float> orig(n);
    for (uint64_t z = 0; z < nz; ++z)
        for (uint64_t y = 0; y < ny; ++y)
            for (uint64_t x = 0; x < nx; ++x)
                orig[(z*ny + y)*nx + x] = ref(x, y, z);
    
    std::string path = std::string("/tmp/be_") + label + ".erwt3d";
    if (!writeERWT3D(path, orig.data(), nx, ny, nz)) {
        std::cerr << label << " write fail" << std::endl;
        ++gFail; return;
    }
    
    ERWT3DReader rp(path), rs(path);
    rs.setIOBackend(IOBackend::Superblock);
    
    for (auto axis : {SliceAxis::X, SliceAxis::Y, SliceAxis::Z}) {
        uint64_t limit = (axis==SliceAxis::X)?nx:(axis==SliceAxis::Y)?ny:nz;
        uint64_t indices[] = {0, limit/2, limit > 0 ? limit-1 : 0};
        for (int k = 0; k < 3; ++k) {
            uint64_t idx = indices[k];
            uint64_t sz = (axis==SliceAxis::X)?ny*nz:(axis==SliceAxis::Y)?nx*nz:nx*ny;
            std::vector<float> o1(sz), o2(sz);
            if (!rp.readSlice(axis, idx, o1.data()) || !rs.readSlice(axis, idx, o2.data())) {
                std::cerr << label << " read fail axis=" << int(axis) << " idx=" << idx << std::endl;
                ++gFail; return;
            }
            for (uint64_t i = 0; i < sz; ++i) {
                if (std::abs(o1[i] - o2[i]) > 1e-6f) {
                    std::cerr << label << " MISMATCH axis=" << int(axis) << " idx=" << idx << " i=" << i << std::endl;
                    ++gFail; return;
                }
            }
        }
    }
    
    std::remove(path.c_str());
    std::cout << "  " << label << ": OK" << std::endl;
}

int main() {
    std::cout << "Backend Equivalence Tests" << std::endl;
    std::cout << "=========================" << std::endl;
    
    testEquiv(17, 19, 23, "17x19x23");
    testEquiv(65, 66, 67, "65x66x67");
    testEquiv(130, 70, 9, "130x70x9");
    testEquiv(9, 130, 70, "9x130x70");
    testEquiv(70, 9, 130, "70x9x130");
    testEquiv(80, 240, 250, "80x240x250");
    testEquiv(100, 100, 100, "100x100x100");
    
    if (gFail) { std::cerr << "\nFAILED" << std::endl; return 1; }
    std::cout << "\nAll backend equivalence tests passed" << std::endl;
    return 0;
}