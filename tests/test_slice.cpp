#include "erwt3d/writer.hpp"
#include "erwt3d/reader.hpp"
#include <iostream>
#include <vector>
#include <cmath>
#include <cassert>
#include <cstdio>

void testSliceZ() {
    std::cout << "Testing Z slice..." << std::endl;
    
    const uint64_t nx = 32, ny = 32, nz = 32;
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
    std::string erwt3dPath = "/tmp/test_slice_z.erwt3d";
    assert(erwt3d::writeERWT3D(erwt3dPath, original.data(), nx, ny, nz));
    
    // Read Z slice
    erwt3d::ERWT3DReader reader(erwt3dPath);
    std::vector<float> slice(nx * ny);
    assert(reader.readSlice(erwt3d::SliceAxis::Z, 10, slice.data()));
    
    // Compare with expected
    for (uint64_t y = 0; y < ny; ++y) {
        for (uint64_t x = 0; x < nx; ++x) {
            uint64_t sliceIdx = y * nx + x;
            uint64_t origIdx = (10 * ny + y) * nx + x;
            assert(std::abs(slice[sliceIdx] - original[origIdx]) < 1e-6);
        }
    }
    
    std::remove(erwt3dPath.c_str());
    std::cout << "  PASSED" << std::endl;
}

void testSliceY() {
    std::cout << "Testing Y slice..." << std::endl;
    
    const uint64_t nx = 32, ny = 32, nz = 32;
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
    std::string erwt3dPath = "/tmp/test_slice_y.erwt3d";
    assert(erwt3d::writeERWT3D(erwt3dPath, original.data(), nx, ny, nz));
    
    // Read Y slice
    erwt3d::ERWT3DReader reader(erwt3dPath);
    std::vector<float> slice(nx * nz);
    assert(reader.readSlice(erwt3d::SliceAxis::Y, 15, slice.data()));
    
    // Compare with expected
    for (uint64_t z = 0; z < nz; ++z) {
        for (uint64_t x = 0; x < nx; ++x) {
            uint64_t sliceIdx = z * nx + x;
            uint64_t origIdx = (z * ny + 15) * nx + x;
            assert(std::abs(slice[sliceIdx] - original[origIdx]) < 1e-6);
        }
    }
    
    std::remove(erwt3dPath.c_str());
    std::cout << "  PASSED" << std::endl;
}

void testSliceX() {
    std::cout << "Testing X slice..." << std::endl;
    
    const uint64_t nx = 32, ny = 32, nz = 32;
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
    std::string erwt3dPath = "/tmp/test_slice_x.erwt3d";
    assert(erwt3d::writeERWT3D(erwt3dPath, original.data(), nx, ny, nz));
    
    // Read X slice
    erwt3d::ERWT3DReader reader(erwt3dPath);
    std::vector<float> slice(ny * nz);
    assert(reader.readSlice(erwt3d::SliceAxis::X, 20, slice.data()));
    
    // Compare with expected
    for (uint64_t z = 0; z < nz; ++z) {
        for (uint64_t y = 0; y < ny; ++y) {
            uint64_t sliceIdx = z * ny + y;
            uint64_t origIdx = (z * ny + y) * nx + 20;
            assert(std::abs(slice[sliceIdx] - original[origIdx]) < 1e-6);
        }
    }
    
    std::remove(erwt3dPath.c_str());
    std::cout << "  PASSED" << std::endl;
}

void testLineX() {
    std::cout << "Testing X line..." << std::endl;
    
    const uint64_t nx = 32, ny = 32, nz = 32;
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
    std::string erwt3dPath = "/tmp/test_line_x.erwt3d";
    assert(erwt3d::writeERWT3D(erwt3dPath, original.data(), nx, ny, nz));
    
    // Read X line
    erwt3d::ERWT3DReader reader(erwt3dPath);
    std::vector<float> line(nx);
    assert(reader.readLineX(10, 20, line.data()));
    
    // Compare with expected
    for (uint64_t x = 0; x < nx; ++x) {
        uint64_t origIdx = (20 * ny + 10) * nx + x;
        assert(std::abs(line[x] - original[origIdx]) < 1e-6);
    }
    
    std::remove(erwt3dPath.c_str());
    std::cout << "  PASSED" << std::endl;
}

int main() {
    std::cout << "Slice Tests" << std::endl;
    std::cout << "===========" << std::endl;
    
    testSliceZ();
    testSliceY();
    testSliceX();
    testLineX();
    
    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}