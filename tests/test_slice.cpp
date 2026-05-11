#include "erwt3d/reader.hpp"
#include "erwt3d/writer.hpp"
#include <iostream>
#include <vector>
#include <cmath>
#include <cstdio>

using namespace erwt3d;

static int gFailures = 0;

static float refVal(uint64_t x, uint64_t y, uint64_t z) {
    return static_cast<float>(x + 1000 * y + 1000000 * z);
}

static std::vector<float> genRef(uint64_t nx, uint64_t ny, uint64_t nz) {
    std::vector<float> data(nx * ny * nz);
    for (uint64_t z = 0; z < nz; ++z)
        for (uint64_t y = 0; y < ny; ++y)
            for (uint64_t x = 0; x < nx; ++x)
                data[(z * ny + y) * nx + x] = refVal(x, y, z);
    return data;
}

#define SL_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "  FAIL: " << msg << std::endl; \
        ++gFailures; \
        return; \
    } \
} while(0)

static void testRoundtrip(uint64_t nx, uint64_t ny, uint64_t nz, const char* label) {
    std::vector<float> orig = genRef(nx, ny, nz);
    std::string path = std::string("/tmp/test_rt_") + label + ".erwt3d";
    
    SL_ASSERT(writeERWT3D(path, orig.data(), nx, ny, nz), "writeERWT3D failed");
    
    ERWT3DReader reader(path);
    std::vector<float> restored(nx * ny * nz);
    SL_ASSERT(reader.readFull(restored.data()), "readFull failed");
    
    for (uint64_t i = 0; i < orig.size(); ++i) {
        SL_ASSERT(std::abs(orig[i] - restored[i]) < 1e-6f, "roundtrip mismatch");
    }
    
    std::remove(path.c_str());
    std::cout << "  Roundtrip " << label << ": OK" << std::endl;
}

static void testSlice(uint64_t nx, uint64_t ny, uint64_t nz, SliceAxis axis,
                       uint64_t idx, const char* label) {
    std::vector<float> orig = genRef(nx, ny, nz);
    std::string path = std::string("/tmp/test_sl_") + label + ".erwt3d";
    
    SL_ASSERT(writeERWT3D(path, orig.data(), nx, ny, nz), "writeERWT3D failed");
    ERWT3DReader reader(path);
    
    uint64_t outSize = 0;
    switch (axis) {
        case SliceAxis::X: outSize = ny * nz; break;
        case SliceAxis::Y: outSize = nx * nz; break;
        case SliceAxis::Z: outSize = nx * ny; break;
    }
    
    std::vector<float> slice(outSize);
    SL_ASSERT(reader.readSlice(axis, idx, slice.data()), "readSlice failed");
    
    switch (axis) {
        case SliceAxis::X:
            for (uint64_t z = 0; z < nz; ++z)
                for (uint64_t y = 0; y < ny; ++y)
                    SL_ASSERT(std::abs(slice[z * ny + y] - refVal(idx, y, z)) < 1e-6f,
                              "X slice value mismatch");
            break;
        case SliceAxis::Y:
            for (uint64_t z = 0; z < nz; ++z)
                for (uint64_t x = 0; x < nx; ++x)
                    SL_ASSERT(std::abs(slice[z * nx + x] - refVal(x, idx, z)) < 1e-6f,
                              "Y slice value mismatch");
            break;
        case SliceAxis::Z:
            for (uint64_t y = 0; y < ny; ++y)
                for (uint64_t x = 0; x < nx; ++x)
                    SL_ASSERT(std::abs(slice[y * nx + x] - refVal(x, y, idx)) < 1e-6f,
                              "Z slice value mismatch");
            break;
    }
    
    std::remove(path.c_str());
}

static void testLineX(uint64_t nx, uint64_t ny, uint64_t nz, uint64_t y, uint64_t z, const char* label) {
    std::vector<float> orig = genRef(nx, ny, nz);
    std::string path = std::string("/tmp/test_lx_") + label + ".erwt3d";
    
    SL_ASSERT(writeERWT3D(path, orig.data(), nx, ny, nz), "writeERWT3D failed");
    ERWT3DReader reader(path);
    
    std::vector<float> line(nx);
    SL_ASSERT(reader.readLineX(y, z, line.data()), "readLineX failed");
    
    for (uint64_t x = 0; x < nx; ++x) {
        SL_ASSERT(std::abs(line[x] - refVal(x, y, z)) < 1e-6f, "lineX value mismatch");
    }
    
    std::remove(path.c_str());
}

static void testAllSlicesAndLines(uint64_t nx, uint64_t ny, uint64_t nz, const char* label) {
    std::string lbl(label);
    
    testSlice(nx, ny, nz, SliceAxis::X, 0, (lbl + "_x0").c_str());
    testSlice(nx, ny, nz, SliceAxis::X, (nx-1)/2, (lbl + "_xmid").c_str());
    testSlice(nx, ny, nz, SliceAxis::X, nx-1, (lbl + "_xmax").c_str());
    
    testSlice(nx, ny, nz, SliceAxis::Y, 0, (lbl + "_y0").c_str());
    testSlice(nx, ny, nz, SliceAxis::Y, (ny-1)/2, (lbl + "_ymid").c_str());
    testSlice(nx, ny, nz, SliceAxis::Y, ny-1, (lbl + "_ymax").c_str());
    
    testSlice(nx, ny, nz, SliceAxis::Z, 0, (lbl + "_z0").c_str());
    testSlice(nx, ny, nz, SliceAxis::Z, (nz-1)/2, (lbl + "_zmid").c_str());
    testSlice(nx, ny, nz, SliceAxis::Z, nz-1, (lbl + "_zmax").c_str());
    
    testLineX(nx, ny, nz, 0, 0, (lbl + "_lx00").c_str());
    testLineX(nx, ny, nz, ny/2, nz/2, (lbl + "_lxmm").c_str());
    testLineX(nx, ny, nz, ny-1, nz-1, (lbl + "_lxmax").c_str());
    
    std::cout << "  Slices+lines " << label << ": OK" << std::endl;
}

int main() {
    std::cout << "Slice Tests (Extended)" << std::endl;
    std::cout << "======================" << std::endl;
    
    testSlice(32, 32, 32, SliceAxis::Z, 10, "z10_32");
    testSlice(32, 32, 32, SliceAxis::Y, 15, "y15_32");
    testSlice(32, 32, 32, SliceAxis::X, 20, "x20_32");
    testLineX(32, 32, 32, 10, 20, "lx_32");
    
    testRoundtrip(17, 19, 23, "17x19x23");
    testAllSlicesAndLines(17, 19, 23, "17x19x23");
    
    testRoundtrip(65, 66, 67, "65x66x67");
    testAllSlicesAndLines(65, 66, 67, "65x66x67");
    
    testRoundtrip(130, 70, 9, "130x70x9");
    testAllSlicesAndLines(130, 70, 9, "130x70x9");
    
    testRoundtrip(9, 130, 70, "9x130x70");
    testAllSlicesAndLines(9, 130, 70, "9x130x70");
    
    testRoundtrip(70, 9, 130, "70x9x130");
    testAllSlicesAndLines(70, 9, 130, "70x9x130");
    
    testRoundtrip(80, 240, 250, "80x240x250");
    testAllSlicesAndLines(80, 240, 250, "80x240x250");
    
    testRoundtrip(8, 8, 8, "8x8x8");
    testRoundtrip(32, 32, 32, "32x32x32");
    
    if (gFailures > 0) {
        std::cerr << "\n" << gFailures << " test(s) FAILED" << std::endl;
        return 1;
    }
    std::cout << "\nAll slice tests passed!" << std::endl;
    return 0;
}