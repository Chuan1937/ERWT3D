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

static void testStorageBytesEmbedded() {
    const char* rawPath = "/tmp/test_embedded_storage.raw";
    const char* outPath = "/tmp/test_embedded_storage.erwt3d";

    const uint64_t nx = 40;
    const uint64_t ny = 30;
    const uint64_t nz = 50;
    {
        std::vector<float> data(nx * ny * nz, 0.0f);
        for (uint64_t x = 0; x < nx; ++x) {
            for (uint64_t y = 0; y < ny; ++y) {
                for (uint64_t z = 0; z < nz; ++z) {
                    data[rawOffsetZFastest(x, y, z, ny, nz)] =
                        static_cast<float>(x * 1000 + y * 10 + z);
                }
            }
        }
        std::ofstream out(rawPath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()),
                  data.size() * sizeof(float));
    }

    CHECK(writeERWT3DFromFile(outPath, rawPath, nx, ny, nz,
                               64, 64, 64, 4, 4, 4, 2, 256,
                               0, 0, true, RawXAuxMode::Off, false),
          "write LZ4 file");

    Lz4XpSidecarStats xpStats;
    CHECK(writeLz4XpSidecar(rawPath, outPath, nx, ny, nz,
                              2, 256, 1.50, true, &xpStats),
          "write embedded XP");

    std::error_code ec;
    CHECK(std::filesystem::is_regular_file(outPath, ec), "main file exists");
    uint64_t mainFileBytes = std::filesystem::file_size(outPath, ec);
    CHECK(!std::filesystem::exists(std::string(outPath) + ".xp", ec),
          "no external .xp file");

    ERWT3DReader reader(outPath);
    const auto& header = reader.getHeader();
    CHECK(hasXPEmbedded(header), "header has embedded XP flag");
    CHECK(hasXP(header), "header has XP flags");
    CHECK(hasXPSidecar(header) || hasXPEmbedded(header),
          "XP support available");

    uint64_t totalBytes = getTotalOptimizedStorageBytes(
        outPath, mainFileBytes, header);
    CHECK(totalBytes == mainFileBytes,
          "storage bytes equals main file only (no external .xp)");

    uint64_t rawBytes = getRawSize(header);
    double ratio = rawBytes > 0 ? static_cast<double>(totalBytes) / rawBytes : 0.0;
    CHECK(ratio > 0.0 && ratio < 2.0, "reasonable storage ratio");

    std::vector<float> dummyX(ny * nz);
    CHECK(reader.readSlice(SliceAxis::X, 0, dummyX.data()), "reader can read X slice");

    unlink(rawPath);
    unlink(outPath);
}

int main() {
    testStorageBytesEmbedded();

    std::cout << "Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed > 0 ? 1 : 0;
}
