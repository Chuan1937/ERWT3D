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

static void testPanel(uint64_t nx, uint64_t ny, uint64_t nz,
                       uint32_t stride, const char* label) {
    uint64_t n = nx * ny * nz;
    std::vector<float> orig(n);
    for (uint64_t z = 0; z < nz; ++z)
        for (uint64_t y = 0; y < ny; ++y)
            for (uint64_t x = 0; x < nx; ++x)
                orig[(z*ny + y)*nx + x] = ref(x, y, z);

    // Write with X-panels
    std::string path = std::string("/tmp/panel_") + label + ".erwt3d";
    if (!writeERWT3D(path, orig.data(), nx, ny, nz,
                     64,64,64, 4,4,4, 1,2048,
                     0, stride)) {
        std::cerr << label << " panel write fail" << std::endl;
        ++gFail; return;
    }

    ERWT3DHeader hdr;
    ERWT3DReader rp(path);
    const auto& h = rp.getHeader();
    if (!hasXPanels(h)) {
        std::cerr << label << " panels NOT detected in header" << std::endl;
        ++gFail; std::remove(path.c_str()); return;
    }

    // Verify panel vs raw for all X slices (hit + miss)
    for (int xi = 0; xi < static_cast<int>(nx); xi += std::max(1, static_cast<int>(nx)/10)) {
        uint64_t idx = static_cast<uint64_t>(xi);
        bool expectHit = (idx % 64 % stride == 0);

        uint64_t sz = ny * nz;
        std::vector<float> panelOut(sz), sbOut(sz);

        // Read with panel-aware SB path
        rp.setIOBackend(IOBackend::Superblock);
        rp.setSBParallelMode(SBParallelMode::Serial);
        if (!rp.readSlice(SliceAxis::X, idx, panelOut.data(), 1, 2048)) {
            std::cerr << label << " panel read fail at x=" << idx << std::endl;
            ++gFail; std::remove(path.c_str()); return;
        }

        // Verify against reference
        for (uint64_t z = 0; z < nz; ++z) {
            for (uint64_t y = 0; y < ny; ++y) {
                float expected = ref(idx, y, z);
                if (std::abs(panelOut[z*ny + y] - expected) > 1e-6f) {
                    std::cerr << label << " PANEL MISMATCH x=" << idx << " y=" << y << " z=" << z
                              << " got=" << panelOut[z*ny + y] << " exp=" << expected << std::endl;
                    ++gFail; std::remove(path.c_str()); return;
                }
            }
        }
    }

    std::remove(path.c_str());
    std::cout << "  " << label << ": OK" << std::endl;
}

int main() {
    std::cout << "Panel Index Tests" << std::endl;
    std::cout << "=================" << std::endl;

    testPanel(100, 100, 100, 4, "100^3_s4");
    testPanel(65, 66, 67, 4, "65x66x67_s4");
    testPanel(130, 70, 9, 4, "130x70x9_s4");
    testPanel(80, 240, 250, 4, "80x240x250_s4");
    testPanel(50, 50, 50, 2, "50^3_s2");
    testPanel(33, 44, 55, 8, "33x44x55_s8");

    if (gFail) { std::cerr << "\nFAILED" << std::endl; return 1; }
    std::cout << "\nAll panel index tests passed" << std::endl;
    return 0;
}
