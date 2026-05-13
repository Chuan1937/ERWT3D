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

static void testLineEquiv(uint64_t nx, uint64_t ny, uint64_t nz,
                           const std::vector<int>& threadCounts,
                           const char* label) {
    uint64_t n = nx * ny * nz;
    std::vector<float> orig(n);
    for (uint64_t z = 0; z < nz; ++z)
        for (uint64_t y = 0; y < ny; ++y)
            for (uint64_t x = 0; x < nx; ++x)
                orig[(z*ny + y)*nx + x] = ref(x, y, z);

    std::string path = std::string("/tmp/line_") + label + ".erwt3d";
    if (!writeERWT3D(path, orig.data(), nx, ny, nz)) {
        std::cerr << label << " write fail" << std::endl; ++gFail; return;
    }

    // Test each axis at boundaries: (f1,f2) = (0,0), (mid,mid), (max-1,max-1)
    for (auto axis : {SliceAxis::X, SliceAxis::Y, SliceAxis::Z}) {
        uint64_t lim1,lim2;
        if (axis == SliceAxis::X) { lim1 = ny; lim2 = nz; }
        else if (axis == SliceAxis::Y) { lim1 = nx; lim2 = nz; }
        else { lim1 = nx; lim2 = ny; }

        uint64_t mid1 = lim1/2, mid2 = lim2/2;
        uint64_t max1 = lim1>0?lim1-1:0, max2 = lim2>0?lim2-1:0;
        uint64_t bounds[3][2] = {{0,0}, {mid1,mid2}, {max1,max2}};

        for (int bi = 0; bi < 3; ++bi) {
            uint64_t f1 = bounds[bi][0], f2 = bounds[bi][1];
            uint64_t sz = (axis==SliceAxis::X)?nx:(axis==SliceAxis::Y)?ny:nz;

            // Test 1: serial SB correctness
            {
                ERWT3DReader r(path); r.setIOBackend(IOBackend::Superblock);
                std::vector<float> out(sz);
                if (!r.readLine(axis, f1, f2, out.data(), 1, 2048)) {
                    std::cerr << label << " " << int(axis) << " serial fail f1=" << f1 << " f2=" << f2 << std::endl;
                    ++gFail; goto cleanup;
                }
                for (uint64_t i = 0; i < sz; ++i) {
                    uint64_t x,y,z;
                    if (axis==SliceAxis::X){x=i;y=f1;z=f2;}
                    else if (axis==SliceAxis::Y){x=f1;y=i;z=f2;}
                    else {x=f1;y=f2;z=i;}
                    if (std::abs(out[i] - ref(x,y,z)) > 1e-6f) {
                        std::cerr << label << " " << int(axis) << " mismatch i=" << i << std::endl;
                        ++gFail; goto cleanup;
                    }
                }
            }

            // Test 2: wrapper == generic API (separate buffers)
            {
                ERWT3DReader r(path); r.setIOBackend(IOBackend::Superblock);
                std::vector<float> outG(sz), outW(sz);
                bool okG = r.readLine(axis, f1, f2, outG.data(), 1, 2048);
                bool okW;
                if (axis == SliceAxis::X) okW = r.readLineX(f1, f2, outW.data(), 1, 2048);
                else if (axis == SliceAxis::Y) okW = r.readLineY(f1, f2, outW.data(), 1, 2048);
                else okW = r.readLineZ(f1, f2, outW.data(), 1, 2048);
                if (!okG || !okW) {
                    std::cerr << label << " " << int(axis) << " wrapper/generic fail" << std::endl;
                    ++gFail; goto cleanup;
                }
                for (uint64_t i = 0; i < sz; ++i) {
                    if (std::abs(outG[i] - outW[i]) > 1e-6f) {
                        std::cerr << label << " " << int(axis) << " wrapper!=generic i=" << i << std::endl;
                        ++gFail; goto cleanup;
                    }
                }
            }
        }
    }

    // Test 3: serial vs parallel-read bit-identical for all axes
    for (int nth : threadCounts) {
        for (auto axis : {SliceAxis::X, SliceAxis::Y, SliceAxis::Z}) {
            uint64_t f1,f2;
            if (axis==SliceAxis::X){f1=ny/2; f2=nz/2;}
            else if (axis==SliceAxis::Y){f1=nx/2; f2=nz/2;}
            else {f1=nx/2; f2=ny/2;}
            uint64_t sz = (axis==SliceAxis::X)?nx:(axis==SliceAxis::Y)?ny:nz;

            ERWT3DReader rs(path); rs.setIOBackend(IOBackend::Superblock);
            std::vector<float> sOut(sz);
            if (!rs.readLine(axis, f1, f2, sOut.data(), 1, 2048)) {
                std::cerr << label << " t=" << nth << " serial fail" << std::endl; ++gFail; goto cleanup;
            }

            ERWT3DReader rp(path); rp.setIOBackend(IOBackend::Superblock);
            rp.setSBParallelMode(SBParallelMode::ParallelRead);
            std::vector<float> pOut(sz);
            if (!rp.readLine(axis, f1, f2, pOut.data(), nth, 2048)) {
                std::cerr << label << " t=" << nth << " parallel fail" << std::endl; ++gFail; goto cleanup;
            }

            for (uint64_t i = 0; i < sz; ++i) {
                if (std::abs(sOut[i] - pOut[i]) > 1e-6f) {
                    std::cerr << label << " t=" << nth << " serial!=parallel axis=" << int(axis)
                              << " i=" << i << " s=" << sOut[i] << " p=" << pOut[i] << std::endl;
                    ++gFail; goto cleanup;
                }
            }
        }
        std::cout << "  " << label << " t=" << nth << " parallel-equiv: OK" << std::endl;
    }

    std::cout << "  " << label << " boundaries+wrappers: OK" << std::endl;
cleanup:
    std::remove(path.c_str());
}

int main() {
    std::cout << "Line Read Tests" << std::endl;
    std::cout << "===============" << std::endl;

    std::vector<int> threads = {2, 4, 8};
    testLineEquiv(17, 19, 23, threads, "17x19x23");
    testLineEquiv(65, 66, 67, threads, "65x66x67");
    testLineEquiv(130, 70, 9, threads, "130x70x9");
    testLineEquiv(9, 130, 70, threads, "9x130x70");
    testLineEquiv(70, 9, 130, threads, "70x9x130");
    testLineEquiv(80, 240, 250, threads, "80x240x250");
    testLineEquiv(100, 100, 100, threads, "100x100x100");

    if (gFail) { std::cerr << "\nFAILED" << std::endl; return 1; }
    std::cout << "\nAll line read tests passed" << std::endl;
    return 0;
}
