#include "erwt3d/writer.hpp"
#include "erwt3d/reader.hpp"
#include <iostream>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <cstring>

using namespace erwt3d;

static int gFail = 0;

static float ref(uint64_t x, uint64_t y, uint64_t z) {
    return static_cast<float>(x + 1000*y + 1000000*z);
}

static void testEquiv(uint64_t nx, uint64_t ny, uint64_t nz,
                       const std::vector<int>& threadCounts,
                       const char* label) {
    uint64_t n = nx * ny * nz;
    std::vector<float> orig(n);
    for (uint64_t z = 0; z < nz; ++z)
        for (uint64_t y = 0; y < ny; ++y)
            for (uint64_t x = 0; x < nx; ++x)
                orig[(z*ny + y)*nx + x] = ref(x, y, z);

    std::string path = std::string("/tmp/sbp_") + label + ".erwt3d";
    if (!writeERWT3D(path, orig.data(), nx, ny, nz)) {
        std::cerr << label << " write fail" << std::endl;
        ++gFail; return;
    }

    for (int nth : threadCounts) {
        ERWT3DReader rs(path);  // serial
        rs.setIOBackend(IOBackend::Superblock);
        rs.setSBParallelMode(SBParallelMode::Serial);

        ERWT3DReader rp(path);  // parallel-read
        rp.setIOBackend(IOBackend::Superblock);
        rp.setSBParallelMode(SBParallelMode::ParallelRead);

        for (auto axis : {SliceAxis::X, SliceAxis::Y, SliceAxis::Z}) {
            uint64_t limit = (axis==SliceAxis::X)?nx:(axis==SliceAxis::Y)?ny:nz;
            uint64_t indices[] = {0, limit/2, limit > 0 ? limit-1 : 0};
            for (int k = 0; k < 3; ++k) {
                uint64_t idx = indices[k];
                uint64_t sz = (axis==SliceAxis::X)?ny*nz:(axis==SliceAxis::Y)?nx*nz:nx*ny;
                std::vector<float> o1(sz), o2(sz);

                if (!rs.readSlice(axis, idx, o1.data(), 1, 2048) ||
                    !rp.readSlice(axis, idx, o2.data(), nth, 2048)) {
                    std::cerr << label << " t=" << nth << " read fail axis="
                              << int(axis) << " idx=" << idx << std::endl;
                    ++gFail; goto cleanup;
                }
                for (uint64_t i = 0; i < sz; ++i) {
                    if (std::abs(o1[i] - o2[i]) > 1e-6f) {
                        std::cerr << label << " t=" << nth << " MISMATCH axis="
                                  << int(axis) << " idx=" << idx << " i=" << i
                                  << " s=" << o1[i] << " p=" << o2[i] << std::endl;
                        ++gFail; goto cleanup;
                    }
                }
            }
        }
        std::cout << "  " << label << " t=" << nth << ": OK" << std::endl;
    }

cleanup:
    std::remove(path.c_str());
}

int main() {
    std::cout << "SB Parallel Mode Equivalence Tests" << std::endl;
    std::cout << "==================================" << std::endl;

    std::vector<int> threads = {2, 4, 8};

    testEquiv(17, 19, 23, threads, "17x19x23");
    testEquiv(65, 66, 67, threads, "65x66x67");
    testEquiv(130, 70, 9, threads, "130x70x9");
    testEquiv(9, 130, 70, threads, "9x130x70");
    testEquiv(70, 9, 130, threads, "70x9x130");
    testEquiv(80, 240, 250, threads, "80x240x250");
    testEquiv(100, 100, 100, threads, "100x100x100");

    if (gFail) { std::cerr << "\nFAILED" << std::endl; return 1; }
    std::cout << "\nAll SB parallel equivalence tests passed" << std::endl;
    return 0;
}
