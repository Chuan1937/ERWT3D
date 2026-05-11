#include "erwt3d/writer.hpp"
#include "erwt3d/reader.hpp"
#include <iostream>
#include <vector>
#include <cmath>
#include <cassert>
#include <fstream>
#include <cstdio>

void testSmallVolume() {
    std::cout << "Testing small volume (8x8x8)..." << std::endl;
    
    const uint64_t nx = 8, ny = 8, nz = 8;
    const uint64_t totalElements = nx * ny * nz;
    
    // Generate test data
    std::vector<float> original(totalElements);
    for (uint64_t z = 0; z < nz; ++z) {
        for (uint64_t y = 0; y < ny; ++y) {
            for (uint64_t x = 0; x < nx; ++x) {
                uint64_t idx = (z * ny + y) * nx + x;
                original[idx] = static_cast<float>(x + 1000 * y + 1000000 * z);
            }
        }
    }
    
    // Write to ERWT3D
    std::string erwt3dPath = "/tmp/test_small.erwt3d";
    assert(erwt3d::writeERWT3D(erwt3dPath, original.data(), nx, ny, nz));
    
    // Read back
    erwt3d::ERWT3DReader reader(erwt3dPath);
    std::vector<float> restored(totalElements);
    assert(reader.readFull(restored.data()));
    
    // Compare
    for (uint64_t i = 0; i < totalElements; ++i) {
        assert(std::abs(original[i] - restored[i]) < 1e-6);
    }
    
    std::remove(erwt3dPath.c_str());
    std::cout << "  PASSED" << std::endl;
}

void testNonAlignedVolume() {
    std::cout << "Testing non-aligned volume (17x19x23)..." << std::endl;
    
    const uint64_t nx = 17, ny = 19, nz = 23;
    const uint64_t totalElements = nx * ny * nz;
    
    // Generate test data
    std::vector<float> original(totalElements);
    for (uint64_t z = 0; z < nz; ++z) {
        for (uint64_t y = 0; y < ny; ++y) {
            for (uint64_t x = 0; x < nx; ++x) {
                uint64_t idx = (z * ny + y) * nx + x;
                original[idx] = static_cast<float>(x + 1000 * y + 1000000 * z);
            }
        }
    }
    
    // Write to ERWT3D
    std::string erwt3dPath = "/tmp/test_nonaligned.erwt3d";
    assert(erwt3d::writeERWT3D(erwt3dPath, original.data(), nx, ny, nz));
    
    // Read back
    erwt3d::ERWT3DReader reader(erwt3dPath);
    std::vector<float> restored(totalElements);
    assert(reader.readFull(restored.data()));
    
    // Compare
    for (uint64_t i = 0; i < totalElements; ++i) {
        assert(std::abs(original[i] - restored[i]) < 1e-6);
    }
    
    std::remove(erwt3dPath.c_str());
    std::cout << "  PASSED" << std::endl;
}

void testLargerVolume() {
    std::cout << "Testing larger volume (65x66x67)..." << std::endl;
    
    const uint64_t nx = 65, ny = 66, nz = 67;
    const uint64_t totalElements = nx * ny * nz;
    
    // Generate test data
    std::vector<float> original(totalElements);
    for (uint64_t z = 0; z < nz; ++z) {
        for (uint64_t y = 0; y < ny; ++y) {
            for (uint64_t x = 0; x < nx; ++x) {
                uint64_t idx = (z * ny + y) * nx + x;
                original[idx] = static_cast<float>(x + 1000 * y + 1000000 * z);
            }
        }
    }
    
    // Write to ERWT3D
    std::string erwt3dPath = "/tmp/test_larger.erwt3d";
    assert(erwt3d::writeERWT3D(erwt3dPath, original.data(), nx, ny, nz));
    
    // Read back
    erwt3d::ERWT3DReader reader(erwt3dPath);
    std::vector<float> restored(totalElements);
    assert(reader.readFull(restored.data()));
    
    // Compare
    for (uint64_t i = 0; i < totalElements; ++i) {
        assert(std::abs(original[i] - restored[i]) < 1e-6);
    }
    
    std::remove(erwt3dPath.c_str());
    std::cout << "  PASSED" << std::endl;
}

int main() {
    std::cout << "Round-trip Tests" << std::endl;
    std::cout << "===============" << std::endl;
    
    testSmallVolume();
    testNonAlignedVolume();
    testLargerVolume();
    
    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}