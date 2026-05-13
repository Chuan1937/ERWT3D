#include "erwt3d/writer.hpp"
#include "erwt3d/reader.hpp"
#include <iostream>
#include <vector>
#include <cmath>
#include <cstring>

using namespace erwt3d;
static int gFail = 0;

static float ref(uint64_t x, uint64_t y, uint64_t z) {
    return static_cast<float>(x + 1000*y + 1000000*z);
}

static void testReadMode(uint64_t nx, uint64_t ny, uint64_t nz, const char* label) {
    uint64_t n = nx * ny * nz;
    std::vector<float> orig(n);
    for (uint64_t z=0;z<nz;++z) for (uint64_t y=0;y<ny;++y) for (uint64_t x=0;x<nx;++x)
        orig[(z*ny+y)*nx+x] = ref(x,y,z);

    std::string path = std::string("/tmp/rm_")+label+".erwt3d";
    if (!writeERWT3D(path, orig.data(), nx, ny, nz)) {
        std::cerr << label << " write fail\n"; ++gFail; return;
    }

    for (auto axis : {SliceAxis::X, SliceAxis::Y, SliceAxis::Z}) {
        uint64_t lim = (axis==SliceAxis::X)?nx:(axis==SliceAxis::Y)?ny:nz;
        uint64_t idxs[] = {0, lim/2, lim>0?lim-1:0};
        for (int k=0;k<3;++k) {
            uint64_t idx = idxs[k];
            uint64_t sz = (axis==SliceAxis::X)?ny*nz:(axis==SliceAxis::Y)?nx*nz:nx*ny;
            std::vector<float> o1(sz), o2(sz);

            ERWT3DReader rp(path); rp.setIOBackend(IOBackend::Superblock);
            if (!rp.readSlice(axis, idx, o1.data(), 1, 2048)) {
                std::cerr << label << " pread fail axis="<<int(axis)<<" idx="<<idx<<"\n"; ++gFail; goto cl;
            }

            ERWT3DReader rb(path); rb.setIOBackend(IOBackend::Superblock);
            rb.setSBReadMode(SBReadMode::RunBatch);
            if (!rb.readSlice(axis, idx, o2.data(), 1, 2048)) {
                std::cerr << label << " run-batch fail axis="<<int(axis)<<" idx="<<idx<<"\n"; ++gFail; goto cl;
            }

            for (uint64_t i=0;i<sz;++i) {
                if (std::abs(o1[i]-o2[i]) > 1e-6f) {
                    std::cerr << label << " mismatch axis="<<int(axis)<<" idx="<<idx<<" i="<<i<<"\n"; ++gFail; goto cl;
                }
            }
        }
    }
    std::cout << "  " << label << ": OK\n";
cl:
    std::remove(path.c_str());
}

int main() {
    std::cout << "SB Read Mode Equivalence\n========================\n";
    testReadMode(17,19,23,"17x19x23");
    testReadMode(130,70,9,"130x70x9");
    if (gFail) { std::cerr << "\nFAILED\n"; return 1; }
    std::cout << "\nAll SB read mode tests passed\n";
    return 0;
}
