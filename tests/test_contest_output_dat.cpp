#include "erwt3d/contest_groups.hpp"
#include "erwt3d/contest_positions.hpp"
#include "erwt3d/writer.hpp"
#include "erwt3d/reader.hpp"
#include "erwt3d/format.hpp"
#include <cassert>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

using namespace erwt3d;

static int testsPassed = 0;
static int testsFailed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " at line " << __LINE__ << "\n"; \
        ++testsFailed; \
    } else { ++testsPassed; } \
} while(0)

static float valueAt(uint64_t x, uint64_t y, uint64_t z) {
    return static_cast<float>(x * 1000000 + y * 1000 + z);
}

static bool writeRawFile(const std::string& path, uint64_t nx, uint64_t ny, uint64_t nz) {
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    uint64_t yz = ny * nz;
    std::vector<float> slab(yz);
    for (uint64_t x = 0; x < nx; ++x) {
        for (uint64_t y = 0; y < ny; ++y)
            for (uint64_t z = 0; z < nz; ++z)
                slab[y * nz + z] = valueAt(x, y, z);
        ssize_t n = write(fd, slab.data(), yz * sizeof(float));
        if (n != static_cast<ssize_t>(yz * sizeof(float))) { close(fd); return false; }
    }
    close(fd);
    return true;
}

static bool readDatFile(const std::string& path, std::vector<float>& out) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    out.resize(st.st_size / sizeof(float));
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = read(fd, out.data(), st.st_size);
    close(fd);
    return n == st.st_size;
}

static void testLZ4OutputDat() {
    const uint64_t nx = 17, ny = 19, nz = 23;
    const std::string rawPath = "/tmp/test_dat_raw.dat";
    const std::string erwtPath = "/tmp/test_dat_lz4.erwt3d";

    CHECK(writeRawFile(rawPath, nx, ny, nz), "write raw");

    bool ok = writeERWT3DFromFile(erwtPath, rawPath, nx, ny, nz,
                                   4, 4, 4, 2, 2, 2, 1, 256, 0, 0, true);
    CHECK(ok, "write ERWT3D LZ4");

    // Generate positions with small count for small data
    ContestPositions positions;
    for (uint64_t i = 0; i < 5; ++i) positions.x_random.push_back(i * 3 % nx);
    for (uint64_t i = 0; i < 5; ++i) positions.y_random.push_back(i * 3 % ny);
    for (uint64_t i = 0; i < 5; ++i) positions.z_random.push_back(i * 3 % nz);
    positions.x_random.push_back(1); positions.x_random.push_back(2);
    positions.y_random.push_back(1); positions.y_random.push_back(2);
    positions.z_random.push_back(1); positions.z_random.push_back(2);
    for (uint64_t i = 0; i < 7; ++i) positions.x_random.push_back(10 + i);
    for (uint64_t i = 0; i < 7; ++i) positions.y_random.push_back(10 + i);
    for (uint64_t i = 0; i < 7; ++i) positions.z_random.push_back(10 + i);
    for (uint64_t i = 0; i < 3; ++i) positions.x_continuous.push_back(5 + i);
    for (uint64_t i = 0; i < 3; ++i) positions.y_continuous.push_back(7 + i);
    for (uint64_t i = 0; i < 3; ++i) positions.z_continuous.push_back(9 + i);

    // Fix: need exactly matching counts for validation-free path
    // Use test interface: generate exactly what we need
    positions.x_random = {0, 3, 6, 9, 12, 15, 1, 2, 10, 11, 13, 14, 4, 5, 7, 8, 16};
    positions.y_random = {0, 3, 6, 9, 12, 15, 1, 2, 10, 11, 13, 14, 4, 5, 7, 8, 16, 18};
    positions.z_random = {0, 3, 6, 9, 12, 15, 1, 2, 10, 11, 13, 14, 4, 5, 7, 8, 16, 18, 20, 22};
    positions.x_continuous = {5, 6, 7};
    positions.y_continuous = {7, 8, 9};
    positions.z_continuous = {9, 10, 11};

    auto reader = std::make_shared<ERWT3DReader>(erwtPath);
    reader->setIOBackend(IOBackend::Superblock);
    reader->setSBReadMode(SBReadMode::HDDReadWindow);

    ContestReadBatchFunction readFn = [reader](
        SliceAxis axis,
        const std::vector<uint64_t>& indices,
        std::vector<std::vector<float>>& outputs
    ) -> bool {
        for (size_t i = 0; i < indices.size(); ++i) {
            if (!reader->readSlice(axis, indices[i], outputs[i].data(), 4, 256))
                return false;
        }
        return true;
    };

    const std::string outDir = "/tmp/test_dat_out_lz4";
    mkdir(outDir.c_str(), 0755);

    ContestUnifiedProfile profile;
    ok = executeContestGroups(positions, outDir, nx, ny, nz, readFn, &profile);
    CHECK(ok, "execute LZ4 groups");

    // Check output file count
    CHECK(profile.output_file_count == positions.x_random.size() + positions.y_random.size() +
          positions.z_random.size() + positions.x_continuous.size() +
          positions.y_continuous.size() + positions.z_continuous.size(),
          "output file count matches");

    // Check file extension and size
    struct stat st;
    std::string xSlicePath = outDir;
    xSlicePath += "/contest_x_random_000.dat";
    CHECK(stat(xSlicePath.c_str(), &st) == 0, "x_random_000.dat exists");
    CHECK(st.st_size == static_cast<off_t>(ny * nz * sizeof(float)), "x slice size = ny*nz*4");

    std::string ySlicePath = outDir;
    ySlicePath += "/contest_y_random_000.dat";
    CHECK(stat(ySlicePath.c_str(), &st) == 0, "y_random_000.dat exists");
    CHECK(st.st_size == static_cast<off_t>(nx * nz * sizeof(float)), "y slice size = nx*nz*4");

    std::string zSlicePath = outDir;
    zSlicePath += "/contest_z_random_000.dat";
    CHECK(stat(zSlicePath.c_str(), &st) == 0, "z_random_000.dat exists");
    CHECK(st.st_size == static_cast<off_t>(nx * ny * sizeof(float)), "z slice size = nx*ny*4");

    // Check content correctness for X slice
    {
        std::vector<float> got;
        CHECK(readDatFile(xSlicePath, got), "read x slice");
        uint64_t x = positions.x_random[0];
        bool match = true;
        for (uint64_t y = 0; y < ny && match; ++y)
            for (uint64_t z = 0; z < nz; ++z)
                if (got[y * nz + z] != valueAt(x, y, z)) { match = false; break; }
        CHECK(match, "x slice content correct");
    }

    // Check content correctness for Y slice
    {
        std::vector<float> got;
        CHECK(readDatFile(ySlicePath, got), "read y slice");
        uint64_t y = positions.y_random[0];
        bool match = true;
        for (uint64_t x = 0; x < nx && match; ++x)
            for (uint64_t z = 0; z < nz; ++z)
                if (got[x * nz + z] != valueAt(x, y, z)) { match = false; break; }
        CHECK(match, "y slice content correct");
    }

    // Check content correctness for Z slice
    {
        std::vector<float> got;
        CHECK(readDatFile(zSlicePath, got), "read z slice");
        uint64_t z = positions.z_random[0];
        bool match = true;
        for (uint64_t x = 0; x < nx && match; ++x)
            for (uint64_t y = 0; y < ny; ++y)
                if (got[x * ny + y] != valueAt(x, y, z)) { match = false; break; }
        CHECK(match, "z slice content correct");
    }

    // Check all times > 0
    CHECK(profile.x_random.time_ms >= 0, "x_random time >= 0");
    CHECK(profile.t_composite_ms >= 0, "t_composite >= 0");

    // Cleanup
    unlink(rawPath.c_str());
    unlink(erwtPath.c_str());
}

int main() {
    testLZ4OutputDat();
    std::cout << "Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed > 0 ? 1 : 0;
}
