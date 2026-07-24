#include "erwt3d/rzfp_writer.hpp"
#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/rzfp_format.hpp"
#include "erwt3d/rzfp_xplane_codec.hpp"
#include "erwt3d/rzfp_axis_plane_writer.hpp"
#include "erwt3d/axis_plane.hpp"
#include "erwt3d/raw_layout.hpp"

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

static const std::string kTmpDir = "/tmp/test_rzfp_yz_plane";
static const uint64_t kNx = 40;
static const uint64_t kNy = 30;
static const uint64_t kNz = 50;

static std::string rawPath() { return kTmpDir + "/data.raw"; }
static std::string rzfpPath() { return kTmpDir + "/data.rzfp"; }

static void prepareInput() {
    std::filesystem::remove_all(kTmpDir);
    std::filesystem::create_directories(kTmpDir);

    {
        std::vector<float> data(kNx * kNy * kNz);
        for (uint64_t x = 0; x < kNx; ++x)
            for (uint64_t y = 0; y < kNy; ++y)
                for (uint64_t z = 0; z < kNz; ++z)
                    data[rawOffsetZFastest(x, y, z, kNy, kNz)] =
                        static_cast<float>(x * 10000 + y * 100 + z);
        std::ofstream out(rawPath(), std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()),
                  data.size() * sizeof(float));
    }

    RzfpWriterConfig wCfg{};
    wCfg.nx = kNx; wCfg.ny = kNy; wCfg.nz = kNz;
    wCfg.super_size = 64; wCfg.leaf_size = 4;
    wCfg.threads = 2;
    wCfg.codec.error.policy = RelativeErrorPolicy::Strict;
    wCfg.codec.error.contest_bound = 0.001;
    CHECK(writeRzfpFile(rawPath(), rzfpPath(), wCfg), "write RZFP file");
}

static RzfpXPlaneCodecConfig codecCfg() {
    RzfpXPlaneCodecConfig c{};
    c.error.policy = RelativeErrorPolicy::Strict;
    c.error.contest_bound = 0.001;
    return c;
}

void testRoundTrip(PlaneAxis axis, uint64_t planeIndex) {
    RzfpAxisPlaneWriterStats stats;
    auto cfg = codecCfg();

    CHECK(writeRzfpAxisPlaneSidecar(rawPath(), rzfpPath(), axis, cfg,
                                     kNx, kNy, kNz, 3, &stats),
          std::string("write ") + axisLabel(axis) + " RZFP sidecar");
    CHECK(stats.written, "stats written");
    CHECK(stats.axis == axis, "stats axis correct");
    CHECK(stats.plane_count > 0, "plane count > 0");

    const std::string sidecarPath = axisPlaneSidecarPath(rzfpPath(), axis);
    CHECK(std::filesystem::exists(sidecarPath), "sidecar file exists");

    // Verify header
    AxisPlaneHeader hdr{};
    {
        std::ifstream in(sidecarPath, std::ios::binary);
        in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
        CHECK(in.good(), "header read");
    }
    CHECK(validateAxisPlaneHeader(hdr, kNx, kNy, kNz), "header valid");
    CHECK(hdr.axis == static_cast<uint8_t>(axis), "header axis");
    CHECK(hdr.compression == AXISPLANE_COMPRESSION_RZFP_2D, "RZFP_2D");

    // Verify index
    std::vector<AxisPlaneIndexEntry> idx(static_cast<size_t>(hdr.plane_count));
    {
        std::ifstream in(sidecarPath, std::ios::binary);
        in.seekg(hdr.index_offset);
        in.read(reinterpret_cast<char*>(idx.data()),
                static_cast<std::streamsize>(hdr.plane_count * sizeof(AxisPlaneIndexEntry)));
        CHECK(in.good(), "index read");
    }

    CHECK(idx[planeIndex].compressed_size > 0, "plane has data");
    CHECK(idx[planeIndex].offset > 0, "plane has valid offset");

    // Decode and verify
    std::vector<uint8_t> record(idx[planeIndex].compressed_size);
    {
        std::ifstream in(sidecarPath, std::ios::binary);
        in.seekg(idx[planeIndex].offset);
        in.read(reinterpret_cast<char*>(record.data()),
                static_cast<std::streamsize>(record.size()));
        CHECK(in.good(), "plane data read");
    }

    const uint64_t planeElements = hdr.plane_elements;
    uint64_t dimA, dimB;
    if (axis == PlaneAxis::Y) { dimA = kNx; dimB = kNz; }
    else { dimA = kNx; dimB = kNy; }

    std::vector<float> plane(static_cast<size_t>(planeElements));
    CHECK(decodeXPlane2D(record.data(), record.size(), plane.data(),
                          dimA, dimB),
          "decodeXPlane2D");

    // Verify values with relaxed error tolerance (RZFP 2D codec uses 1e-3)
    int violations = 0;
    double maxRelErr = 0.0;

    if (axis == PlaneAxis::Y) {
        for (uint64_t x = 0; x < kNx; ++x) {
            for (uint64_t z = 0; z < kNz; ++z) {
                float expected = static_cast<float>(x * 10000 + planeIndex * 100 + z);
                float got = plane[x * kNz + z];
                double denom = std::max(std::abs(static_cast<double>(expected)), 1e-10);
                double relErr = std::abs(static_cast<double>(got - expected)) / denom;
                maxRelErr = std::max(maxRelErr, relErr);
                if (relErr > 0.001) ++violations;
            }
        }
    } else {
        for (uint64_t x = 0; x < kNx; ++x) {
            for (uint64_t y = 0; y < kNy; ++y) {
                float expected = static_cast<float>(x * 10000 + y * 100 + planeIndex);
                float got = plane[x * kNy + y];
                double denom = std::max(std::abs(static_cast<double>(expected)), 1e-10);
                double relErr = std::abs(static_cast<double>(got - expected)) / denom;
                maxRelErr = std::max(maxRelErr, relErr);
                if (relErr > 0.001) ++violations;
            }
        }
    }

    CHECK(violations == 0, "no relative error violations");
    CHECK(maxRelErr < 0.001, "max_relative_error < 1e-3");
}

void testReaderRoundTrip() {
    // The sidecars already exist from testRoundTrip calls.
    // Open the reader and verify it detects and uses them.
    RzfpReader reader(rzfpPath());
    CHECK(reader.ok(), "reader opens");

    CHECK(!reader.hasAxisSidecar(PlaneAxis::X), "no X sidecar");
    CHECK(reader.hasAxisSidecar(PlaneAxis::Y), "reader detects Y sidecar");
    CHECK(reader.hasAxisSidecar(PlaneAxis::Z), "reader detects Z sidecar");

    // Read Y=5 through the reader (via .yp sidecar)
    std::vector<float> outY(kNx * kNz);
    CHECK(reader.readSlice(SliceAxis::Y, 5, outY.data()), "read Y=5 via sidecar");

    int yv = 0;
    for (uint64_t x = 0; x < kNx; ++x) {
        for (uint64_t z = 0; z < kNz; ++z) {
            float expected = static_cast<float>(x * 10000 + 5 * 100 + z);
            float got = outY[x * kNz + z];
            double denom = std::max(std::abs(static_cast<double>(expected)), 1e-10);
            double relErr = std::abs(static_cast<double>(got - expected)) / denom;
            if (relErr > 0.001 && yv < 3) { yv++; }
        }
    }
    CHECK(yv == 0, "reader Y values correct");

    // Read Z=3 through the reader (via .zp sidecar)
    std::vector<float> outZ(kNx * kNy);
    CHECK(reader.readSlice(SliceAxis::Z, 3, outZ.data()), "read Z=3 via sidecar");

    int zv = 0;
    for (uint64_t x = 0; x < kNx; ++x) {
        for (uint64_t y = 0; y < kNy; ++y) {
            float expected = static_cast<float>(x * 10000 + y * 100 + 3);
            float got = outZ[x * kNy + y];
            double denom = std::max(std::abs(static_cast<double>(expected)), 1e-10);
            double relErr = std::abs(static_cast<double>(got - expected)) / denom;
            if (relErr > 0.001 && zv < 3) { zv++; }
        }
    }
    CHECK(zv == 0, "reader Z values correct");

    // Mixed batch: X + Y + Z simultaneously
    std::vector<float> outX(kNy * kNz);
    std::vector<RzfpReader::SliceBatchRequest> batch = {
        {SliceAxis::X, 0, outX.data()},
        {SliceAxis::Y, 5, outY.data()},
        {SliceAxis::Z, 3, outZ.data()},
    };

    HDDReadWindowConfig wcfg;
    wcfg.read_window_bytes = 128 * 1024 * 1024;
    wcfg.max_gap_bytes = 8 * 1024 * 1024;
    CHECK(reader.readSlicesBatch(batch, 2, 256, wcfg), "mixed batch");

    // Verify X from batch
    int xv = 0;
    for (uint64_t y = 0; y < kNy; ++y) {
        for (uint64_t z = 0; z < kNz; ++z) {
            float expected = static_cast<float>(0 * 10000 + y * 100 + z);
            float got = outX[y * kNz + z];
            double denom = std::max(std::abs(static_cast<double>(expected)), 1e-10);
            double relErr = std::abs(static_cast<double>(got - expected)) / denom;
            if (relErr > 0.001 && xv < 3) { xv++; }
        }
    }
    CHECK(xv == 0, "batch X correct");

    // Verify Y from batch
    yv = 0;
    for (uint64_t x = 0; x < kNx; ++x) {
        for (uint64_t z = 0; z < kNz; ++z) {
            float expected = static_cast<float>(x * 10000 + 5 * 100 + z);
            float got = outY[x * kNz + z];
            double denom = std::max(std::abs(static_cast<double>(expected)), 1e-10);
            double relErr = std::abs(static_cast<double>(got - expected)) / denom;
            if (relErr > 0.001 && yv < 3) { yv++; }
        }
    }
    CHECK(yv == 0, "batch Y correct");

    // Verify Z from batch
    zv = 0;
    for (uint64_t x = 0; x < kNx; ++x) {
        for (uint64_t y = 0; y < kNy; ++y) {
            float expected = static_cast<float>(x * 10000 + y * 100 + 3);
            float got = outZ[x * kNy + y];
            double denom = std::max(std::abs(static_cast<double>(expected)), 1e-10);
            double relErr = std::abs(static_cast<double>(got - expected)) / denom;
            if (relErr > 0.001 && zv < 3) { zv++; }
        }
    }
    CHECK(zv == 0, "batch Z correct");
}

int main() {
    prepareInput();
    testRoundTrip(PlaneAxis::Y, 5);
    testRoundTrip(PlaneAxis::Z, 3);
    testReaderRoundTrip();
    std::filesystem::remove_all(kTmpDir);

    std::cout << "Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed > 0 ? 1 : 0;
}
