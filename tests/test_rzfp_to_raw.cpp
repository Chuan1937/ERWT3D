#include "erwt3d/rzfp_writer.hpp"
#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/rzfp_format.hpp"
#include "erwt3d/relative_error.hpp"
#include "erwt3d/raw_layout.hpp"

#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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

static void generateDeterministicRaw(
    const std::string& path,
    uint64_t nx, uint64_t ny, uint64_t nz)
{
    std::vector<float> data(nx * ny * nz);
    for (uint64_t x = 0; x < nx; ++x) {
        for (uint64_t y = 0; y < ny; ++y) {
            for (uint64_t z = 0; z < nz; ++z) {
                float val = static_cast<float>(x) * 100.0f +
                            static_cast<float>(y) * 10.0f +
                            static_cast<float>(z);
                data[rawOffsetZFastest(x, y, z, ny, nz)] = val;
            }
        }
    }
    {
        data[rawOffsetZFastest(0, 0, 0, ny, nz)] = 0.0f;
        data[rawOffsetZFastest(1, 0, 0, ny, nz)] = 1e-6f;
        data[rawOffsetZFastest(2, 0, 0, ny, nz)] = -1e-6f;
        data[rawOffsetZFastest(3, 0, 0, ny, nz)] = 42.0f;
        data[rawOffsetZFastest(4, 0, 0, ny, nz)] = -42.0f;
    }
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()),
              data.size() * sizeof(float));
}

static void testRoundtrip() {
    const char* rawPath = "/tmp/test_rzfp_roundtrip.raw";
    const char* rzfpPath = "/tmp/test_rzfp_roundtrip.rzfp";
    const char* restoredPath = "/tmp/test_rzfp_roundtrip_restored.dat";

    const uint64_t nx = 64;
    const uint64_t ny = 64;
    const uint64_t nz = 64;
    generateDeterministicRaw(rawPath, nx, ny, nz);

    RzfpWriterConfig cfg;
    cfg.nx = nx; cfg.ny = ny; cfg.nz = nz;
    cfg.threads = 2;
    cfg.memory_limit_mb = 1024;
    cfg.codec.error.policy = RelativeErrorPolicy::Strict;
    cfg.codec.error.contest_bound = 1e-3;
    cfg.codec.error.internal_bound = 7.5e-4;
    cfg.physical_order = PhysicalOrder::ZYX;

    RzfpWriterStats stats{};
    CHECK(writeRzfpFile(rawPath, rzfpPath, cfg, &stats), "write RZFP file");
    CHECK(stats.violation_count == 0, "no violations in encoding");
    CHECK(stats.max_relative_error < 1e-3, "max error within bounds");

    RzfpReader reader(rzfpPath);
    CHECK(reader.ok(), "open RZFP reader");
    CHECK(reader.header().nx == nx, "nx preserved");
    CHECK(reader.header().ny == ny, "ny preserved");
    CHECK(reader.header().nz == nz, "nz preserved");

    RzfpReaderConfig rcfg;
    rcfg.decode_threads = 2;
    CHECK(reader.readFullToFile(restoredPath, rcfg), "restore to raw");

    std::vector<float> original(nx * ny * nz);
    std::vector<float> restored(nx * ny * nz);
    {
        std::ifstream in(rawPath, std::ios::binary);
        in.read(reinterpret_cast<char*>(original.data()),
                original.size() * sizeof(float));
    }
    {
        std::ifstream in(restoredPath, std::ios::binary);
        in.read(reinterpret_cast<char*>(restored.data()),
                restored.size() * sizeof(float));
    }

    uint64_t violations = 0;
    double maxRelErr = 0.0;
    for (uint64_t i = 0; i < original.size(); ++i) {
        float orig = original[i];
        float rest = restored[i];
        if (orig == 0.0f) {
            if (rest != 0.0f) {
                ++violations;
                if (violations <= 3) {
                    std::cerr << "  Zero value violated at i=" << i
                              << ": restored=" << rest << "\n";
                }
            }
        } else {
            double relErr = std::abs(static_cast<double>(rest - orig)) /
                            std::max(1e-12, std::abs(static_cast<double>(orig)));
            if (relErr > maxRelErr) maxRelErr = relErr;
            if (relErr >= 1e-3) {
                ++violations;
                if (violations <= 3) {
                    std::cerr << "  Error violation at i=" << i
                              << ": orig=" << orig << " restored=" << rest
                              << " rel_err=" << relErr << "\n";
                }
            }
        }
    }

    CHECK(violations == 0, "full restore: zero violations");
    CHECK(maxRelErr < 1e-3, "full restore: max error < 1e-3");

    std::error_code ec;
    CHECK(std::filesystem::is_regular_file(restoredPath, ec), "output file exists");
    auto fsize = std::filesystem::file_size(restoredPath, ec);
    CHECK(fsize == nx * ny * nz * sizeof(float), "output file correct size");

    unlink(rawPath);
    unlink(rzfpPath);
    unlink(restoredPath);
}

static void testSpecialValues() {
    const char* rawPath = "/tmp/test_special.raw";
    const char* rzfpPath = "/tmp/test_special.rzfp";
    const char* restoredPath = "/tmp/test_special_restored.dat";

    const uint64_t nx = 8;
    const uint64_t ny = 8;
    const uint64_t nz = 8;
    {
        std::vector<float> data(nx * ny * nz, 0.0f);
        data[rawOffsetZFastest(0, 0, 0, ny, nz)] = 0.0f;
        data[rawOffsetZFastest(1, 0, 0, ny, nz)] = 1e-7f;
        data[rawOffsetZFastest(2, 0, 0, ny, nz)] = -1e-7f;
        data[rawOffsetZFastest(3, 0, 0, ny, nz)] = 3.14159f;
        data[rawOffsetZFastest(4, 0, 0, ny, nz)] = -3.14159f;
        data[rawOffsetZFastest(0, 7, 0, ny, nz)] = 100.0f;
        data[rawOffsetZFastest(7, 0, 7, ny, nz)] = -200.0f;
        std::ofstream out(rawPath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()),
                  data.size() * sizeof(float));
    }

    RzfpWriterConfig cfg;
    cfg.nx = nx; cfg.ny = ny; cfg.nz = nz;
    cfg.threads = 2;
    cfg.memory_limit_mb = 256;
    cfg.codec.error.policy = RelativeErrorPolicy::Strict;
    cfg.codec.error.contest_bound = 1e-3;
    cfg.codec.error.internal_bound = 7.5e-4;

    RzfpWriterStats stats{};
    CHECK(writeRzfpFile(rawPath, rzfpPath, cfg, &stats), "write RZFP");

    RzfpReader reader(rzfpPath);
    CHECK(reader.ok(), "open RZFP");

    RzfpReaderConfig rcfg;
    rcfg.decode_threads = 2;
    CHECK(reader.readFullToFile(restoredPath, rcfg), "restore");

    std::vector<float> original(nx * ny * nz);
    std::vector<float> restored(nx * ny * nz);
    {
        std::ifstream in(rawPath, std::ios::binary);
        in.read(reinterpret_cast<char*>(original.data()),
                original.size() * sizeof(float));
    }
    {
        std::ifstream in(restoredPath, std::ios::binary);
        in.read(reinterpret_cast<char*>(restored.data()),
                restored.size() * sizeof(float));
    }

    for (uint64_t i = 0; i < original.size(); ++i) {
        if (original[i] == 0.0f) {
            CHECK(restored[i] == 0.0f, "zero preserved");
        } else {
            double relErr = std::abs(static_cast<double>(restored[i] - original[i])) /
                            std::max(1e-12, std::abs(static_cast<double>(original[i])));
            CHECK(relErr < 1e-3, "non-zero within tolerance");
        }
    }

    unlink(rawPath);
    unlink(rzfpPath);
    unlink(restoredPath);
}

int main() {
    testRoundtrip();
    testSpecialValues();

    std::cout << "Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed > 0 ? 1 : 0;
}
