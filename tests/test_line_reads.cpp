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

static void testLine(uint64_t nx, uint64_t ny, uint64_t nz, const char* label) {
    uint64_t n = nx * ny * nz;
    std::vector<float> orig(n);
    for (uint64_t z = 0; z < nz; ++z)
        for (uint64_t y = 0; y < ny; ++y)
            for (uint64_t x = 0; x < nx; ++x)
                orig[(z*ny + y)*nx + x] = ref(x, y, z);

    std::string path = std::string("/tmp/line_") + label + ".erwt3d";
    if (!writeERWT3D(path, orig.data(), nx, ny, nz)) {
        std::cerr << label << " write fail" << std::endl;
        ++gFail; return;
    }

    // Test X-line
    {
        uint64_t y = ny/2, z = nz/2;
        if (y < ny && z < nz) {
            ERWT3DReader r(path); r.setIOBackend(IOBackend::Superblock);
            std::vector<float> outX(nx);
            if (!r.readLineX(y, z, outX.data())) { std::cerr << label << " readLineX fail" << std::endl; ++gFail; goto cleanup; }
            // Also test generic readLine
            std::vector<float> outG(nx);
            if (!r.readLine(SliceAxis::X, y, z, outG.data())) { std::cerr << label << " readLine(X) fail" << std::endl; ++gFail; goto cleanup; }
            for (uint64_t x = 0; x < nx; ++x) {
                float e = ref(x, y, z);
                if (std::abs(outX[x] - e) > 1e-6f) { std::cerr << label << " X mismatch x=" << x << std::endl; ++gFail; goto cleanup; }
                if (std::abs(outG[x] - e) > 1e-6f) { std::cerr << label << " readLine(X) mismatch x=" << x << std::endl; ++gFail; goto cleanup; }
            }
        }
    }

    // Test Y-line
    {
        uint64_t x = nx/2, z = nz/2;
        if (x < nx && z < nz) {
            ERWT3DReader r(path); r.setIOBackend(IOBackend::Superblock);
            std::vector<float> out(ny);
            if (!r.readLineY(x, z, out.data())) { std::cerr << label << " readLineY fail" << std::endl; ++gFail; goto cleanup; }
            if (!r.readLine(SliceAxis::Y, x, z, out.data())) { std::cerr << label << " readLine(Y) fail" << std::endl; ++gFail; goto cleanup; }
            for (uint64_t y = 0; y < ny; ++y) {
                if (std::abs(out[y] - ref(x, y, z)) > 1e-6f) { std::cerr << label << " Y mismatch y=" << y << std::endl; ++gFail; goto cleanup; }
            }
        }
    }

    // Test Z-line
    {
        uint64_t x = nx/2, y = ny/2;
        if (x < nx && y < ny) {
            ERWT3DReader r(path); r.setIOBackend(IOBackend::Superblock);
            std::vector<float> out(nz);
            if (!r.readLineZ(x, y, out.data())) { std::cerr << label << " readLineZ fail" << std::endl; ++gFail; goto cleanup; }
            if (!r.readLine(SliceAxis::Z, x, y, out.data())) { std::cerr << label << " readLine(Z) fail" << std::endl; ++gFail; goto cleanup; }
            for (uint64_t z = 0; z < nz; ++z) {
                if (std::abs(out[z] - ref(x, y, z)) > 1e-6f) { std::cerr << label << " Z mismatch z=" << z << std::endl; ++gFail; goto cleanup; }
            }
        }
    }

    // Test boundary positions
    for (auto axis : {SliceAxis::X, SliceAxis::Y, SliceAxis::Z}) {
        uint64_t lim = (axis==SliceAxis::X)?nx:(axis==SliceAxis::Y)?ny:nz;
        for (uint64_t idx : {uint64_t(0), lim>0?lim-1:uint64_t(0)}) {
            uint64_t f1=0, f2=0;
            if (axis==SliceAxis::X) { f1=ny/2; f2=nz/2; }
            else if (axis==SliceAxis::Y) { f1=nx/2; f2=nz/2; }
            else { f1=nx/2; f2=ny/2; }
            ERWT3DReader r(path); r.setIOBackend(IOBackend::Superblock);
            if (axis==SliceAxis::X && f1>=ny && f2>=nz) continue;
            if (axis==SliceAxis::Y && f1>=nx && f2>=nz) continue;
            if (axis==SliceAxis::Z && f1>=nx && f2>=ny) continue;
            uint64_t sz = (axis==SliceAxis::X)?nx:(axis==SliceAxis::Y)?ny:nz;
            std::vector<float> out(sz);
            if (!r.readLine(axis, f1, f2, out.data())) { std::cerr << label << " readLine fail" << std::endl; ++gFail; goto cleanup; }
        }
    }

    std::cout << "  " << label << ": OK" << std::endl;
cleanup:
    std::remove(path.c_str());
}

int main() {
    std::cout << "Line Read Tests" << std::endl;
    std::cout << "===============" << std::endl;

    testLine(17, 19, 23, "17x19x23");
    testLine(65, 66, 67, "65x66x67");
    testLine(130, 70, 9, "130x70x9");
    testLine(9, 130, 70, "9x130x70");
    testLine(70, 9, 130, "70x9x130");
    testLine(80, 240, 250, "80x240x250");
    testLine(100, 100, 100, "100x100x100");

    if (gFail) { std::cerr << "\nFAILED" << std::endl; return 1; }
    std::cout << "\nAll line read tests passed" << std::endl;
    return 0;
}
