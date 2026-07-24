#include "erwt3d/rzfp_writer.hpp"
#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/rzfp_format.hpp"
#include "erwt3d/rzfp_xplane_writer.hpp"
#include "erwt3d/rzfp_xplane_codec.hpp"
#include "erwt3d/raw_layout.hpp"

#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>

using namespace erwt3d;

static int testsPassed = 0;
static int testsFailed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " at line " << __LINE__ << "\n"; \
        ++testsFailed; \
    } else { ++testsPassed; } \
} while(0)

static std::string TMPDIR = "/tmp/test_rzfp_xp_stability";

static void ensureTmpDir() {
    std::filesystem::remove_all(TMPDIR);
    std::filesystem::create_directories(TMPDIR);
}

static void cleanup() {
    std::filesystem::remove_all(TMPDIR);
}

static std::string rawPath() { return TMPDIR + "/data.raw"; }
static std::string rzfpPath() { return TMPDIR + "/data.rzfp"; }
static std::string xpPath() { return rzfpPath() + ".xp"; }

static const uint64_t NX = 40;
static const uint64_t NY = 30;
static const uint64_t NZ = 50;

static void writeTestRaw() {
    std::vector<float> data(NX * NY * NZ);
    for (uint64_t x = 0; x < NX; ++x) {
        for (uint64_t y = 0; y < NY; ++y) {
            for (uint64_t z = 0; z < NZ; ++z) {
                data[rawOffsetZFastest(x, y, z, NY, NZ)] =
                    static_cast<float>(x * 10000 + y * 100 + z);
            }
        }
    }
    std::ofstream out(rawPath(), std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), data.size() * sizeof(float));
}

static std::vector<float> readRawXPlane(uint64_t x) {
    std::vector<float> raw(NY * NZ);
    std::ifstream in(rawPath(), std::ios::binary);
    std::vector<float> full(NX * NY * NZ);
    in.read(reinterpret_cast<char*>(full.data()), full.size() * sizeof(float));
    for (uint64_t y = 0; y < NY; ++y) {
        for (uint64_t z = 0; z < NZ; ++z) {
            raw[y * NZ + z] = full[rawOffsetZFastest(x, y, z, NY, NZ)];
        }
    }
    return raw;
}

static bool verifyXSlice(const std::vector<float>& got, uint64_t x) {
    auto expected = readRawXPlane(x);
    if (got.size() != expected.size()) return false;
    for (size_t i = 0; i < got.size(); ++i) {
        float diff = std::abs(got[i] - expected[i]);
        float maxAbs = std::max(std::abs(expected[i]), 1e-6f);
        if (diff / maxAbs > 1e-3f) return false;
    }
    return true;
}

static void writeRzfpFile() {
    RzfpWriterConfig cfg{};
    cfg.nx = NX; cfg.ny = NY; cfg.nz = NZ;
    cfg.super_size = 64;
    cfg.leaf_size = 4;
    cfg.threads = 2;
    cfg.codec.error.policy = RelativeErrorPolicy::Strict;
    cfg.codec.error.contest_bound = 0.001;
    CHECK(writeRzfpFile(rawPath(), rzfpPath(), cfg),
          "write RZFP file");
}

static void writeXPSidecar() {
    RzfpXPlaneCodecConfig xpCfg{};
    xpCfg.error.policy = RelativeErrorPolicy::Strict;
    xpCfg.error.contest_bound = 0.001;
    CHECK(writeXPlaneSidecarFile(rawPath(), xpPath(), xpCfg, NX, NY, NZ, 2),
          "write XP sidecar");
}

// Test 1: XP header validation
static void test1_XPHeaderValidation() {
    ensureTmpDir();
    writeTestRaw();
    writeRzfpFile();
    writeXPSidecar();
    CHECK(std::filesystem::exists(xpPath()), ".xp file exists");

    RzfpReader reader(rzfpPath());
    CHECK(reader.hasXPlaneSidecar(), "reader detects sidecar");

    // Verify header
    const auto& h = reader.header();
    CHECK(h.nx == NX && h.ny == NY && h.nz == NZ, "reader dims correct");
}

// Test 2: Index range validation
static void test2_IndexRangeValidation() {
    ensureTmpDir();
    writeTestRaw();
    writeRzfpFile();
    writeXPSidecar();

    RzfpReader reader(rzfpPath());
    CHECK(reader.hasXPlaneSidecar(), "reader detects sidecar");

    // Valid X planes
    std::vector<float> out(NY * NZ);
    CHECK(reader.readSlice(SliceAxis::X, 0, out.data()), "X=0 valid");
    CHECK(reader.readSlice(SliceAxis::X, NX - 1, out.data()), "X=NX-1 valid");
}

// Test 3: Batch X random
static void test3_BatchXRandom() {
    ensureTmpDir();
    writeTestRaw();
    writeRzfpFile();
    writeXPSidecar();

    RzfpReader reader(rzfpPath());
    CHECK(reader.hasXPlaneSidecar(), "reader detects sidecar");

    std::vector<float> out0(NY * NZ), out1(NY * NZ), out2(NY * NZ);
    std::vector<RzfpReader::SliceBatchRequest> requests = {
        {SliceAxis::X, 5, out0.data()},
        {SliceAxis::X, 10, out1.data()},
        {SliceAxis::X, 15, out2.data()},
    };

    HDDReadWindowConfig wcfg;
    wcfg.read_window_bytes = 128 * 1024 * 1024;
    wcfg.max_gap_bytes = 8 * 1024 * 1024;
    CHECK(reader.readSlicesBatch(requests, 2, 256, wcfg),
          "batch X random");

    CHECK(verifyXSlice(out0, 5), "X=5 correct");
    CHECK(verifyXSlice(out1, 10), "X=10 correct");
    CHECK(verifyXSlice(out2, 15), "X=15 correct");
}

// Test 4: Batch X continuous
static void test4_BatchXContinuous() {
    ensureTmpDir();
    writeTestRaw();
    writeRzfpFile();
    writeXPSidecar();

    RzfpReader reader(rzfpPath());
    CHECK(reader.hasXPlaneSidecar(), "reader detects sidecar");

    std::vector<std::vector<float>> outs(10);
    std::vector<RzfpReader::SliceBatchRequest> requests;
    for (int i = 0; i < 10; ++i) {
        outs[i].resize(NY * NZ);
        requests.push_back({SliceAxis::X, static_cast<uint64_t>(i), outs[i].data()});
    }

    HDDReadWindowConfig wcfg;
    wcfg.read_window_bytes = 128 * 1024 * 1024;
    wcfg.max_gap_bytes = 8 * 1024 * 1024;
    CHECK(reader.readSlicesBatch(requests, 2, 256, wcfg),
          "batch X continuous");

    for (int i = 0; i < 10; ++i) {
        CHECK(verifyXSlice(outs[i], static_cast<uint64_t>(i)),
              ("X=" + std::to_string(i) + " correct").c_str());
    }
}

// Test 5: Duplicate X coordinate — should read+decode once, fan out
static void test5_DuplicateXFanOut() {
    ensureTmpDir();
    writeTestRaw();
    writeRzfpFile();
    writeXPSidecar();

    RzfpReader reader(rzfpPath());
    CHECK(reader.hasXPlaneSidecar(), "reader detects sidecar");

    std::vector<float> out0(NY * NZ), out1(NY * NZ);
    std::vector<RzfpReader::SliceBatchRequest> requests = {
        {SliceAxis::X, 3, out0.data()},
        {SliceAxis::X, 3, out1.data()},
    };

    HDDReadWindowConfig wcfg;
    wcfg.read_window_bytes = 128 * 1024 * 1024;
    wcfg.max_gap_bytes = 8 * 1024 * 1024;
    CHECK(reader.readSlicesBatch(requests, 2, 256, wcfg),
          "duplicate X batch");

    CHECK(verifyXSlice(out0, 3), "first X=3 correct");
    CHECK(verifyXSlice(out1, 3), "second X=3 correct");
}

// Test 6: Corrupt sidecar — fallback to main RZFP format
static void test6_CorruptSidecarFallback() {
    ensureTmpDir();
    writeTestRaw();
    writeRzfpFile();
    writeXPSidecar();

    // Corrupt the .xp header
    {
        int fd = open(xpPath().c_str(), O_WRONLY);
        if (fd >= 0) {
            char bad = 'X';
            (void)pwrite(fd, &bad, 1, 0);
            close(fd);
        }
    }

    RzfpReader reader(rzfpPath());
    CHECK(!reader.hasXPlaneSidecar(), "corrupt sidecar rejected");

    // Should still be able to read via main RZFP format
    std::vector<float> out(NY * NZ);
    CHECK(reader.readSlice(SliceAxis::X, 0, out.data()), "read via fallback");
    CHECK(verifyXSlice(out, 0), "X=0 correct via fallback");
}

// Test 7: Missing sidecar — works via main format
static void test7_MissingSidecarWorks() {
    ensureTmpDir();
    writeTestRaw();
    writeRzfpFile();

    RzfpReader reader(rzfpPath());
    CHECK(!reader.hasXPlaneSidecar(), "no sidecar detected");

    std::vector<float> out(NY * NZ);
    CHECK(reader.readSlice(SliceAxis::X, 0, out.data()), "read via main format");
    CHECK(verifyXSlice(out, 0), "X=0 correct");
}

// Test 8: Y/Z requests not affected by XP
static void test8_YZNotAffected() {
    ensureTmpDir();
    writeTestRaw();
    writeRzfpFile();
    writeXPSidecar();

    RzfpReader reader(rzfpPath());
    CHECK(reader.hasXPlaneSidecar(), "reader detects sidecar");

    // Y request should route to main RZFP, not sidecar
    std::vector<float> outY(NX * NZ);
    CHECK(reader.readSlice(SliceAxis::Y, 5, outY.data()), "Y read works");

    // Z request should route to main RZFP
    std::vector<float> outZ(NX * NY);
    CHECK(reader.readSlice(SliceAxis::Z, 3, outZ.data()), "Z read works");

    // Verify Y slice
    for (uint64_t x = 0; x < NX; ++x) {
        for (uint64_t z = 0; z < NZ; ++z) {
            float expected = static_cast<float>(x * 10000 + 5 * 100 + z);
            float diff = std::abs(outY[x * NZ + z] - expected);
            float denom = std::max(std::abs(expected), 1e-6f);
            CHECK(diff / denom < 1e-3f, "Y value correct");
        }
    }

    // Verify Z slice
    for (uint64_t x = 0; x < NX; ++x) {
        for (uint64_t y = 0; y < NY; ++y) {
            float expected = static_cast<float>(x * 10000 + y * 100 + 3);
            float diff = std::abs(outZ[x * NY + y] - expected);
            float denom = std::max(std::abs(expected), 1e-6f);
            CHECK(diff / denom < 1e-3f, "Z value correct");
        }
    }
}

// Test 9: Error tolerance — max_relative_error < 1e-3, exact zero
static void test9_ErrorTolerance() {
    ensureTmpDir();
    writeTestRaw();
    writeRzfpFile();
    writeXPSidecar();

    RzfpReader reader(rzfpPath());
    CHECK(reader.hasXPlaneSidecar(), "reader detects sidecar");

    std::vector<float> out(NY * NZ);
    CHECK(reader.readSlice(SliceAxis::X, 0, out.data()), "read X=0");

    auto expected = readRawXPlane(0);
    double maxRelErr = 0.0;
    int violationCount = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        if (expected[i] == 0.0f && out[i] == 0.0f) continue;
        if (expected[i] == 0.0f && out[i] != 0.0f) {
            ++violationCount;
            continue;
        }
        if (!std::isfinite(expected[i]) || !std::isfinite(out[i])) continue;
        double relErr = std::abs(static_cast<double>(out[i] - expected[i])) /
                        std::max(std::abs(static_cast<double>(expected[i])), 1e-10);
        maxRelErr = std::max(maxRelErr, relErr);
        if (relErr > 0.001) ++violationCount;
    }
    CHECK(violationCount == 0, "no relative error violations");
    CHECK(maxRelErr < 0.001, "max_relative_error < 1e-3");
}

// Test 10: Mixed axes batch — X uses sidecar, Y/Z use main format
static void test10_MixedAxesBatch() {
    ensureTmpDir();
    writeTestRaw();
    writeRzfpFile();
    writeXPSidecar();

    RzfpReader reader(rzfpPath());
    CHECK(reader.hasXPlaneSidecar(), "reader detects sidecar");

    std::vector<float> outX(NY * NZ), outY(NX * NZ), outZ(NX * NY);
    std::vector<RzfpReader::SliceBatchRequest> requests = {
        {SliceAxis::X, 0, outX.data()},
        {SliceAxis::Y, 5, outY.data()},
        {SliceAxis::Z, 3, outZ.data()},
    };

    HDDReadWindowConfig wcfg;
    wcfg.read_window_bytes = 128 * 1024 * 1024;
    wcfg.max_gap_bytes = 8 * 1024 * 1024;
    CHECK(reader.readSlicesBatch(requests, 2, 256, wcfg),
          "mixed axes batch");

    CHECK(verifyXSlice(outX, 0), "X via sidecar");

    for (uint64_t x = 0; x < NX; ++x) {
        for (uint64_t z = 0; z < NZ; ++z) {
            float expected = static_cast<float>(x * 10000 + 5 * 100 + z);
            float diff = std::abs(outY[x * NZ + z] - expected);
            float denom = std::max(std::abs(expected), 1e-6f);
            CHECK(diff / denom < 1e-3f, "Y correct");
        }
    }
    for (uint64_t x = 0; x < NX; ++x) {
        for (uint64_t y = 0; y < NY; ++y) {
            float expected = static_cast<float>(x * 10000 + y * 100 + 3);
            float diff = std::abs(outZ[x * NY + y] - expected);
            float denom = std::max(std::abs(expected), 1e-6f);
            CHECK(diff / denom < 1e-3f, "Z correct");
        }
    }
}

int main() {
    test1_XPHeaderValidation();
    test2_IndexRangeValidation();
    test3_BatchXRandom();
    test4_BatchXContinuous();
    test5_DuplicateXFanOut();
    test6_CorruptSidecarFallback();
    test7_MissingSidecarWorks();
    test8_YZNotAffected();
    test9_ErrorTolerance();
    test10_MixedAxesBatch();

    cleanup();

    std::cout << "Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed > 0 ? 1 : 0;
}
