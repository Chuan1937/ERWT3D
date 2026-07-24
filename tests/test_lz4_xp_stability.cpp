#include "erwt3d/writer.hpp"
#include "erwt3d/reader.hpp"
#include "erwt3d/format.hpp"
#include "erwt3d/lz4_xp_sidecar.hpp"
#include "erwt3d/file_format_detect.hpp"
#include "erwt3d/raw_layout.hpp"

#include <cassert>
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

static std::string TMPDIR = "/tmp/test_lz4_xp_stability";

static void ensureTmpDir() {
    std::filesystem::remove_all(TMPDIR);
    std::filesystem::create_directories(TMPDIR);
}

static void cleanup() {
    std::filesystem::remove_all(TMPDIR);
}

static std::string rawPath() { return TMPDIR + "/data.raw"; }
static std::string lz4Path() { return TMPDIR + "/data.erwt3d"; }
static std::string xpPath() { return lz4Path() + ".xp"; }

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
    for (uint64_t y = 0; y < NY; ++y) {
        for (uint64_t z = 0; z < NZ; ++z) {
            raw[y * NZ + z] = 0;
        }
    }
    std::ifstream in2(rawPath(), std::ios::binary);
    std::vector<float> full(NX * NY * NZ);
    in2.read(reinterpret_cast<char*>(full.data()), full.size() * sizeof(float));
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
        if (std::abs(got[i] - expected[i]) > 1e-6f) return false;
    }
    return true;
}

// Test 1: XP sidecar standalone (external .xp file)
static void test1_XPSidecarStandalone() {
    ensureTmpDir();
    writeTestRaw();

    CHECK(writeERWT3DFromFile(lz4Path(), rawPath(), NX, NY, NZ,
                              64, 64, 64, 4, 4, 4, 2, 256,
                              0, 0, true, RawXAuxMode::Off, false),
          "write LZ4 file");

    Lz4XpSidecarStats stats;
    CHECK(writeLz4XpSidecar(rawPath(), lz4Path(), NX, NY, NZ, 2, 256, 1.50, false, &stats),
          "write external XP sidecar");
    CHECK(std::filesystem::exists(xpPath()), ".xp file exists");

    ERWT3DReader reader(lz4Path());
    const auto& h = reader.getHeader();
    CHECK(hasXPSidecar(h) && !hasXPEmbedded(h), "XP sidecar flag set, embedded not");
    CHECK(hasXP(h), "has XP");

    std::vector<float> out(NY * NZ);
    CHECK(reader.readSlice(SliceAxis::X, 0, out.data()), "read X=0 via XP");
    CHECK(verifyXSlice(out, 0), "X=0 correct");
    CHECK(reader.readSlice(SliceAxis::X, 2, out.data()), "read X=2 via XP");
    CHECK(verifyXSlice(out, 2), "X=2 correct");
}

// Test 2: XP embedded (in .erwt3d file, no external .xp)
static void test2_XPEmbedded() {
    ensureTmpDir();
    writeTestRaw();

    CHECK(writeERWT3DFromFile(lz4Path(), rawPath(), NX, NY, NZ,
                              64, 64, 64, 4, 4, 4, 2, 256,
                              0, 0, true, RawXAuxMode::Off, false),
          "write LZ4 file");

    Lz4XpSidecarStats stats;
    CHECK(writeLz4XpSidecar(rawPath(), lz4Path(), NX, NY, NZ, 2, 256, 1.50, true, &stats),
          "write embedded XP");
    CHECK(!std::filesystem::exists(xpPath()), "no external .xp file");

    ERWT3DReader reader(lz4Path());
    const auto& h = reader.getHeader();
    CHECK(hasXPEmbedded(h), "embedded XP flag set");

    std::vector<float> out(NY * NZ);
    CHECK(reader.readSlice(SliceAxis::X, 0, out.data()), "read X=0 via embedded XP");
    CHECK(verifyXSlice(out, 0), "X=0 correct");
}

// Test 3: Auto discovery - XP works for X, not for Y/Z
static void test3_AutoDiscovery() {
    ensureTmpDir();
    writeTestRaw();

    CHECK(writeERWT3DFromFile(lz4Path(), rawPath(), NX, NY, NZ,
                              64, 64, 64, 4, 4, 4, 2, 256,
                              0, 0, true, RawXAuxMode::Off, false),
          "write LZ4 file");
    Lz4XpSidecarStats stats3;
    CHECK(writeLz4XpSidecar(rawPath(), lz4Path(), NX, NY, NZ, 2, 256, 1.50, false, &stats3),
          "write XP sidecar");

    ERWT3DReader reader(lz4Path());
    const auto& h = reader.getHeader();
    CHECK(hasXP(h), "auto discovers XP");

    // X random: should use XP
    std::vector<float> outX(NY * NZ);
    CHECK(reader.readSlice(SliceAxis::X, 0, outX.data()), "X random via XP");
    CHECK(verifyXSlice(outX, 0), "X random correct");

    // X continuous: should use XP
    CHECK(reader.readSlice(SliceAxis::X, 2, outX.data()), "X continuous via XP");
    CHECK(verifyXSlice(outX, 2), "X continuous correct");

    // Y: should NOT use XP, but should still work
    std::vector<float> outY(NX * NZ);
    CHECK(reader.readSlice(SliceAxis::Y, 5, outY.data()), "Y works");
    // Verify Y slice by comparing with raw
    for (uint64_t x = 0; x < NX; ++x) {
        for (uint64_t z = 0; z < NZ; ++z) {
            float expected = static_cast<float>(x * 10000 + 5 * 100 + z);
            CHECK(std::abs(outY[x * NZ + z] - expected) < 1e-6f, "Y value correct");
        }
    }

    // Z: should NOT use XP
    std::vector<float> outZ(NX * NY);
    CHECK(reader.readSlice(SliceAxis::Z, 3, outZ.data()), "Z works");
}

// Test 4: XP missing — safe fallback to main format
static void test4_XPMissingFallback() {
    ensureTmpDir();
    writeTestRaw();

    CHECK(writeERWT3DFromFile(lz4Path(), rawPath(), NX, NY, NZ,
                              64, 64, 64, 4, 4, 4, 2, 256,
                              0, 0, true, RawXAuxMode::Off, false),
          "write LZ4 file (no XP)");
    CHECK(!std::filesystem::exists(xpPath()), "no .xp file");

    ERWT3DReader reader(lz4Path());
    const auto& h = reader.getHeader();
    CHECK(!hasXP(h), "no XP detected");

    std::vector<float> out(NY * NZ);
    CHECK(reader.readSlice(SliceAxis::X, 0, out.data()), "X=0 via main format");
    CHECK(verifyXSlice(out, 0), "X=0 correct");
}

// Test 5: XP corruption — reject and safe fallback
static void test5_XPCorruptFallback() {
    ensureTmpDir();
    writeTestRaw();

    CHECK(writeERWT3DFromFile(lz4Path(), rawPath(), NX, NY, NZ,
                              64, 64, 64, 4, 4, 4, 2, 256,
                              0, 0, true, RawXAuxMode::Off, false),
          "write LZ4 file");
    CHECK(writeLz4XpSidecar(rawPath(), lz4Path(), NX, NY, NZ, 2, 256, 1.50, false, nullptr),
          "write XP sidecar");

    // Corrupt the .xp file header magic
    {
        int fd = open(xpPath().c_str(), O_WRONLY);
        if (fd >= 0) {
            char bad = 'X';
            (void)pwrite(fd, &bad, 1, 0);
            close(fd);
        }
    }

    // Reader should still open (fallback) and read via main format
    ERWT3DReader reader(lz4Path());
    const auto& h = reader.getHeader();
    CHECK(hasXPSidecar(h), "header still has XP flag"); // flag is in main file, not in .xp

    std::vector<float> out(NY * NZ);
    CHECK(reader.readSlice(SliceAxis::X, 0, out.data()), "X=0 via fallback (corrupt .xp)");
    CHECK(verifyXSlice(out, 0), "X=0 correct despite corrupt .xp");
}

// Test 6: Duplicate X coordinate in batch — read once, fan out
static void test6_DuplicateXFanOut() {
    ensureTmpDir();
    writeTestRaw();

    CHECK(writeERWT3DFromFile(lz4Path(), rawPath(), NX, NY, NZ,
                              64, 64, 64, 4, 4, 4, 2, 256,
                              0, 0, true, RawXAuxMode::Off, false),
          "write LZ4 file");
    CHECK(writeLz4XpSidecar(rawPath(), lz4Path(), NX, NY, NZ, 2, 256, 1.50, false, nullptr),
          "write XP sidecar");

    ERWT3DReader reader(lz4Path());

    // Create batch with duplicate X=0 and X=2 requests
    std::vector<float> out1(NY * NZ), out2(NY * NZ), out3(NY * NZ);
    std::vector<ERWT3DReader::SliceBatchRequest> requests = {
        {SliceAxis::X, 0, out1.data()},
        {SliceAxis::X, 0, out2.data()},
        {SliceAxis::X, 2, out3.data()},
    };

    HDDReadWindowConfig wcfg;
    wcfg.read_window_bytes = 64 * 1024 * 1024;
    wcfg.max_gap_bytes = 4 * 1024 * 1024;
    CHECK(reader.readSlicesBatch(requests, 1, 256, wcfg),
          "batch read with duplicates");

    CHECK(verifyXSlice(out1, 0), "out1 (X=0 first) correct");
    CHECK(verifyXSlice(out2, 0), "out2 (X=0 second) correct");
    CHECK(verifyXSlice(out3, 2), "out3 (X=2) correct");
}

// Test 7: Contiguous X-plane reading — should use merged pread
static void test7_ContinuousX() {
    ensureTmpDir();
    writeTestRaw();

    CHECK(writeERWT3DFromFile(lz4Path(), rawPath(), NX, NY, NZ,
                              64, 64, 64, 4, 4, 4, 2, 256,
                              0, 0, true, RawXAuxMode::Off, false),
          "write LZ4 file");
    CHECK(writeLz4XpSidecar(rawPath(), lz4Path(), NX, NY, NZ, 2, 256, 1.50, false, nullptr),
          "write XP sidecar");

    ERWT3DReader reader(lz4Path());

    // 10 continuous X planes
    std::vector<std::vector<float>> outs(10);
    std::vector<ERWT3DReader::SliceBatchRequest> requests;
    for (int i = 0; i < 10; ++i) {
        outs[i].resize(NY * NZ);
        requests.push_back({SliceAxis::X, static_cast<uint64_t>(i * 2), outs[i].data()});
    }

    HDDReadWindowConfig wcfg;
    wcfg.read_window_bytes = 64 * 1024 * 1024;
    wcfg.max_gap_bytes = 4 * 1024 * 1024;
    CHECK(reader.readSlicesBatch(requests, 1, 256, wcfg),
          "batch read continuous X");

    for (int i = 0; i < 10; ++i) {
        CHECK(verifyXSlice(outs[i], static_cast<uint64_t>(i * 2)),
              ("X=" + std::to_string(i * 2) + " correct").c_str());
    }
}

// Test 8: Stride handling — only stride-aligned X use XP
static void test8_StrideHandling() {
    ensureTmpDir();
    writeTestRaw();

    CHECK(writeERWT3DFromFile(lz4Path(), rawPath(), NX, NY, NZ,
                              64, 64, 64, 4, 4, 4, 2, 256,
                              0, 0, true, RawXAuxMode::Off, false),
          "write LZ4 file");
    CHECK(writeLz4XpSidecar(rawPath(), lz4Path(), NX, NY, NZ, 2, 256, 1.50, false, nullptr),
          "write XP sidecar (stride=2)");

    ERWT3DReader reader(lz4Path());

    // X=0 (stride-aligned) should use XP
    std::vector<float> out0(NY * NZ);
    CHECK(reader.readSlice(SliceAxis::X, 0, out0.data()), "X=0 via XP");
    CHECK(verifyXSlice(out0, 0), "X=0 correct");

    // X=1 (not stride-aligned) should fall back to main format
    std::vector<float> out1(NY * NZ);
    CHECK(reader.readSlice(SliceAxis::X, 1, out1.data()), "X=1 via main format");
    CHECK(verifyXSlice(out1, 1), "X=1 correct");
}

// Test 9: Batch with mixed axes — only X uses XP, Y/Z use SB
static void test9_MixedAxesBatch() {
    ensureTmpDir();
    writeTestRaw();

    CHECK(writeERWT3DFromFile(lz4Path(), rawPath(), NX, NY, NZ,
                              64, 64, 64, 4, 4, 4, 2, 256,
                              0, 0, true, RawXAuxMode::Off, false),
          "write LZ4 file");
    CHECK(writeLz4XpSidecar(rawPath(), lz4Path(), NX, NY, NZ, 2, 256, 1.50, false, nullptr),
          "write XP sidecar");

    ERWT3DReader reader(lz4Path());

    std::vector<float> outX(NY * NZ), outY(NX * NZ), outZ(NX * NY);
    std::vector<ERWT3DReader::SliceBatchRequest> requests = {
        {SliceAxis::X, 0, outX.data()},
        {SliceAxis::Y, 5, outY.data()},
        {SliceAxis::Z, 3, outZ.data()},
    };

    HDDReadWindowConfig wcfg;
    wcfg.read_window_bytes = 64 * 1024 * 1024;
    wcfg.max_gap_bytes = 4 * 1024 * 1024;
    CHECK(reader.readSlicesBatch(requests, 1, 256, wcfg),
          "mixed axes batch");

    CHECK(verifyXSlice(outX, 0), "X via XP");
    for (uint64_t x = 0; x < NX; ++x) {
        for (uint64_t z = 0; z < NZ; ++z) {
            float expected = static_cast<float>(x * 10000 + 5 * 100 + z);
            CHECK(std::abs(outY[x * NZ + z] - expected) < 1e-6f, "Y correct");
        }
    }
    for (uint64_t x = 0; x < NX; ++x) {
        for (uint64_t y = 0; y < NY; ++y) {
            float expected = static_cast<float>(x * 10000 + y * 100 + 3);
            CHECK(std::abs(outZ[x * NY + y] - expected) < 1e-6f, "Z correct");
        }
    }
}

int main() {
    test1_XPSidecarStandalone();
    test2_XPEmbedded();
    test3_AutoDiscovery();
    test4_XPMissingFallback();
    test5_XPCorruptFallback();
    test6_DuplicateXFanOut();
    test7_ContinuousX();
    test8_StrideHandling();
    test9_MixedAxesBatch();

    cleanup();

    std::cout << "Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed > 0 ? 1 : 0;
}
