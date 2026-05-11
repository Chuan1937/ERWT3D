#include "erwt3d/writer.hpp"
#include "erwt3d/reader.hpp"
#include <iostream>
#include <vector>
#include <cmath>
#include <cstdio>

using namespace erwt3d;

static int gFailures = 0;

static float refVal(uint64_t x, uint64_t y, uint64_t z) {
    return static_cast<float>(x + 1000 * y + 1000000 * z);
}

#define RT_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "  FAIL: " << msg << std::endl; \
        ++gFailures; \
        return; \
    } \
} while(0)

static void testVolume(uint64_t nx, uint64_t ny, uint64_t nz, const char* label) {
    std::vector<float> original(nx * ny * nz);
    for (uint64_t z = 0; z < nz; ++z)
        for (uint64_t y = 0; y < ny; ++y)
            for (uint64_t x = 0; x < nx; ++x)
                original[(z * ny + y) * nx + x] = refVal(x, y, z);
    
    std::string path = std::string("/tmp/test_rt_") + label + ".erwt3d";
    RT_ASSERT(writeERWT3D(path, original.data(), nx, ny, nz), "writeERWT3D failed");
    
    ERWT3DReader reader(path);
    std::vector<float> restored(nx * ny * nz);
    RT_ASSERT(reader.readFull(restored.data()), "readFull failed");
    
    for (uint64_t i = 0; i < original.size(); ++i) {
        if (std::abs(original[i] - restored[i]) >= 1e-6f) {
            uint64_t x = i % nx;
            uint64_t yz = i / nx;
            uint64_t y = yz % ny;
            uint64_t z = yz / ny;
            std::cerr << "  FAIL [" << label << "] at (" << x << "," << y << "," << z << "): "
                      << "orig=" << original[i] << " got=" << restored[i] << std::endl;
            ++gFailures;
            std::remove(path.c_str());
            return;
        }
    }
    
    std::remove(path.c_str());
    std::cout << "  " << label << ": OK" << std::endl;
}

int main() {
    std::cout << "Round-trip Tests" << std::endl;
    std::cout << "===============" << std::endl;
    
    testVolume(8, 8, 8, "8x8x8");
    testVolume(17, 19, 23, "17x19x23");
    testVolume(65, 66, 67, "65x66x67");
    testVolume(130, 70, 9, "130x70x9");
    testVolume(9, 130, 70, "9x130x70");
    testVolume(70, 9, 130, "70x9x130");
    testVolume(80, 240, 250, "80x240x250");
    testVolume(63, 64, 65, "63x64x65");
    testVolume(64, 64, 64, "64x64x64");
    testVolume(64, 64, 128, "64x64x128");
    testVolume(128, 64, 64, "128x64x64");
    testVolume(64, 128, 64, "64x128x64");
    
    if (gFailures > 0) {
        std::cerr << "\n" << gFailures << " test(s) FAILED" << std::endl;
        return 1;
    }
    std::cout << "\nAll roundtrip tests passed!" << std::endl;
    return 0;
}