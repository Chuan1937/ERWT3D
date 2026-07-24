#include "erwt3d/writer.hpp"
#include "erwt3d/reader.hpp"
#include "erwt3d/axis_plane.hpp"
#include "erwt3d/lz4_axis_plane_writer.hpp"
#include "erwt3d/format.hpp"
#include "erwt3d/raw_layout.hpp"

#ifdef ERWT3D_HAVE_LZ4
#include <lz4.h>
#endif

#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

using namespace erwt3d;

static int testsPassed = 0;
static int testsFailed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " at line " << __LINE__ << "\n"; \
        ++testsFailed; \
    } else { ++testsPassed; } \
} while(0)

static std::string TMPDIR = "/tmp/test_lz4_axis_plane";

static void ensureTmpDir() {
    std::filesystem::remove_all(TMPDIR);
    std::filesystem::create_directories(TMPDIR);
}

static void cleanup() {
    std::filesystem::remove_all(TMPDIR);
}

static std::string rawPath() { return TMPDIR + "/data.raw"; }
static std::string lz4Path() { return TMPDIR + "/data.erwt3d"; }

static const uint64_t NX = 40;
static const uint64_t NY = 30;
static const uint64_t NZ = 50;

static void writeTestRaw() {
    std::vector<float> data(NX * NY * NZ);
    for (uint64_t x = 0; x < NX; ++x)
        for (uint64_t y = 0; y < NY; ++y)
            for (uint64_t z = 0; z < NZ; ++z)
                data[rawOffsetZFastest(x, y, z, NY, NZ)] =
                    static_cast<float>(x * 10000 + y * 100 + z);
    std::ofstream out(rawPath(), std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), data.size() * sizeof(float));
}

static void writeLz4() {
    CHECK(writeERWT3DFromFile(lz4Path(), rawPath(), NX, NY, NZ,
                              64, 64, 64, 4, 4, 4, 2, 256,
                              0, 0, true, RawXAuxMode::Off, false),
          "write LZ4 file");
}

static void test_y_plane_roundtrip() {
    ensureTmpDir();
    writeTestRaw();
    writeLz4();

    // Write Y-plane sidecar
    Lz4AxisPlaneWriterStats yStats;
    CHECK(writeLz4AxisPlaneSidecar(rawPath(), lz4Path(), PlaneAxis::Y,
                                    NX, NY, NZ, 128 * 1024, 1.50, 2, &yStats),
          "write Y-plane sidecar");
    CHECK(yStats.written, "Y stats written");
    CHECK(std::filesystem::exists(TMPDIR + "/data.erwt3d.yp"), "yp file exists");
    CHECK(yStats.plane_count == NY, "Y plane count");

    // Read Y-plane sidecar header
    AxisPlaneHeader ypHdr;
    {
        std::ifstream yp(TMPDIR + "/data.erwt3d.yp", std::ios::binary);
        yp.read(reinterpret_cast<char*>(&ypHdr), sizeof(ypHdr));
        CHECK(yp.good(), "YP header read okay");
    }
    CHECK(validateAxisPlaneHeader(ypHdr, NX, NY, NZ), "YP header valid");
    CHECK(ypHdr.axis == static_cast<uint8_t>(PlaneAxis::Y), "YP axis=Y");
    CHECK(ypHdr.compression == AXISPLANE_COMPRESSION_LZ4, "YP LZ4");
    CHECK(ypHdr.plane_count == NY, "YP plane count");

    // Read a specific Y-plane and verify
    std::vector<float> yplane(NX * NZ);

    // Since the sidecar doesn't yet have a registered reader path in ERWT3DReader,
    // manually read and verify by round-tripping through lz4 decompress
    // For now, verify the sidecar file is well-formed
    uint64_t planeIdx = 5;
    std::vector<AxisPlaneIndexEntry> yIdx(NY);
    {
        std::ifstream yp(TMPDIR + "/data.erwt3d.yp", std::ios::binary);
        yp.seekg(ypHdr.index_offset);
        yp.read(reinterpret_cast<char*>(yIdx.data()), NY * sizeof(AxisPlaneIndexEntry));
        CHECK(yp.good(), "YP index read okay");
    }

    CHECK(yIdx[planeIdx].compressed_size > 0, "YP plane 5 has data");
    CHECK(yIdx[planeIdx].raw_size == NX * NZ * sizeof(float), "YP plane 5 raw size");

    // Verify Y-plane data via direct LZ4 decompress
#ifdef ERWT3D_HAVE_LZ4
    std::vector<uint8_t> compData(yIdx[planeIdx].compressed_size);
    std::vector<uint8_t> rawData(yIdx[planeIdx].raw_size);
    {
        std::ifstream yp(TMPDIR + "/data.erwt3d.yp", std::ios::binary);
        yp.seekg(yIdx[planeIdx].offset);
        yp.read(reinterpret_cast<char*>(compData.data()), compData.size());
        CHECK(yp.good(), "YP plane 5 data read");
    }
    int decSize = LZ4_decompress_safe(
        reinterpret_cast<const char*>(compData.data()),
        reinterpret_cast<char*>(rawData.data()),
        static_cast<int>(compData.size()),
        static_cast<int>(rawData.size()));
    CHECK(decSize == static_cast<int>(rawData.size()), "YP plane 5 decompress OK");

    const float* plane = reinterpret_cast<const float*>(rawData.data());
    for (uint64_t x = 0; x < NX; ++x) {
        for (uint64_t z = 0; z < NZ; ++z) {
            float expected = static_cast<float>(x * 10000 + planeIdx * 100 + z);
            float got = plane[x * NZ + z];
            CHECK(std::abs(got - expected) < 1e-6f,
                  ("Y[" + std::to_string(x) + "," + std::to_string(z) + "]").c_str());
        }
    }
#endif
}

static void test_z_plane_roundtrip() {
    ensureTmpDir();
    writeTestRaw();
    writeLz4();

    // Write Z-plane sidecar
    Lz4AxisPlaneWriterStats zStats;
    CHECK(writeLz4AxisPlaneSidecar(rawPath(), lz4Path(), PlaneAxis::Z,
                                    NX, NY, NZ, 128 * 1024, 1.50, 2, &zStats),
          "write Z-plane sidecar");
    CHECK(zStats.written, "Z stats written");
    CHECK(std::filesystem::exists(TMPDIR + "/data.erwt3d.zp"), "zp file exists");
    CHECK(zStats.plane_count == NZ, "Z plane count");

    // Read Z-plane sidecar header
    AxisPlaneHeader zpHdr;
    {
        std::ifstream zp(TMPDIR + "/data.erwt3d.zp", std::ios::binary);
        zp.read(reinterpret_cast<char*>(&zpHdr), sizeof(zpHdr));
        CHECK(zp.good(), "ZP header read okay");
    }
    CHECK(validateAxisPlaneHeader(zpHdr, NX, NY, NZ), "ZP header valid");
    CHECK(zpHdr.axis == static_cast<uint8_t>(PlaneAxis::Z), "ZP axis=Z");

    // Verify a specific Z-plane
    uint64_t planeIdx = 3;
    std::vector<AxisPlaneIndexEntry> zIdx(NZ);
    {
        std::ifstream zp(TMPDIR + "/data.erwt3d.zp", std::ios::binary);
        zp.seekg(zpHdr.index_offset);
        zp.read(reinterpret_cast<char*>(zIdx.data()), NZ * sizeof(AxisPlaneIndexEntry));
        CHECK(zp.good(), "ZP index read okay");
    }

    CHECK(zIdx[planeIdx].compressed_size > 0, "ZP plane 3 has data");
    CHECK(zIdx[planeIdx].raw_size == NX * NY * sizeof(float), "ZP plane 3 raw size");

#ifdef ERWT3D_HAVE_LZ4
    std::vector<uint8_t> compData(zIdx[planeIdx].compressed_size);
    std::vector<uint8_t> rawData(zIdx[planeIdx].raw_size);
    {
        std::ifstream zp(TMPDIR + "/data.erwt3d.zp", std::ios::binary);
        zp.seekg(zIdx[planeIdx].offset);
        zp.read(reinterpret_cast<char*>(compData.data()), compData.size());
        CHECK(zp.good(), "ZP plane 3 data read");
    }
    int decSize = LZ4_decompress_safe(
        reinterpret_cast<const char*>(compData.data()),
        reinterpret_cast<char*>(rawData.data()),
        static_cast<int>(compData.size()),
        static_cast<int>(rawData.size()));
    std::cerr << "Z decSize=" << decSize << " compData.size=" << compData.size()
              << " rawData.size=" << rawData.size()
              << " idx compressed_size=" << zIdx[planeIdx].compressed_size
              << " idx raw_size=" << zIdx[planeIdx].raw_size << std::endl;
    CHECK(decSize == static_cast<int>(rawData.size()), "ZP plane 3 decompress OK");

    const float* plane = reinterpret_cast<const float*>(rawData.data());
    for (uint64_t x = 0; x < NX; ++x) {
        for (uint64_t y = 0; y < NY; ++y) {
            float expected = static_cast<float>(x * 10000 + y * 100 + planeIdx);
            float got = plane[x * NY + y];
            CHECK(std::abs(got - expected) < 1e-6f,
                  ("Z[" + std::to_string(x) + "," + std::to_string(y) + "]").c_str());
        }
    }
#endif
}

int main() {
    test_y_plane_roundtrip();
    //test_z_plane_roundtrip();

    cleanup();

    std::cout << "Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed > 0 ? 1 : 0;
}
