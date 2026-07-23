#include "erwt3d/rzfp_writer.hpp"
#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/rzfp_format.hpp"
#include "erwt3d/relative_error.hpp"
#include "erwt3d/raw_layout.hpp"

#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <unistd.h>
#include <unordered_set>

using namespace erwt3d;

static int testsPassed = 0;
static int testsFailed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " at line " << __LINE__ << "\n"; \
        ++testsFailed; \
    } else { ++testsPassed; } \
} while(0)

static void writeRawFile(const std::string& path,
                          uint64_t nx, uint64_t ny, uint64_t nz)
{
    std::vector<float> data(nx * ny * nz);
    for (uint64_t x = 0; x < nx; ++x) {
        for (uint64_t y = 0; y < ny; ++y) {
            for (uint64_t z = 0; z < nz; ++z) {
                float val = static_cast<float>((x * ny + y) * nz + z);
                data[rawOffsetZFastest(x, y, z, ny, nz)] = val;
            }
        }
    }
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()),
              data.size() * sizeof(float));
}

static void testLineX() {
    const char* rawPath = "/tmp/test_line_x.raw";
    const char* rzfpPath = "/tmp/test_line_x.rzfp";

    const uint64_t nx = 33;
    const uint64_t ny = 25;
    const uint64_t nz = 41;
    writeRawFile(rawPath, nx, ny, nz);

    RzfpWriterConfig cfg;
    cfg.nx = nx; cfg.ny = ny; cfg.nz = nz;
    cfg.threads = 2;
    cfg.memory_limit_mb = 256;
    cfg.codec.error.policy = RelativeErrorPolicy::Strict;
    cfg.codec.error.contest_bound = 1e-3;
    cfg.codec.error.internal_bound = 7.5e-4;
    CHECK(writeRzfpFile(rawPath, rzfpPath, cfg), "write RZFP");

    RzfpReader reader(rzfpPath);
    CHECK(reader.ok(), "open reader");

    RzfpReaderConfig rcfg;
    rcfg.decode_threads = 2;

    {
        std::vector<float> line(nx);
        CHECK(reader.readLineX(0, 0, line.data(), rcfg), "lineX y=0,z=0");
        for (uint64_t x = 0; x < nx; ++x) {
            float expected = static_cast<float>((x * ny + 0) * nz + 0);
            if (expected == 0.0f) {
                CHECK(line[x] == 0.0f, "lineX zero preserved");
            } else {
                double relErr = std::abs(static_cast<double>(line[x] - expected))
                                / std::abs(static_cast<double>(expected));
                CHECK(relErr < 1e-3, "lineX non-zero within tolerance");
            }
        }
    }

    {
        std::vector<float> line(nx);
        CHECK(reader.readLineX(12, 20, line.data(), rcfg), "lineX y=12,z=20");
        for (uint64_t x = 0; x < nx; ++x) {
            float expected = static_cast<float>((x * ny + 12) * nz + 20);
            if (expected == 0.0f) {
                CHECK(line[x] == 0.0f, "lineX zero preserved");
            } else {
                double relErr = std::abs(static_cast<double>(line[x] - expected))
                                / std::abs(static_cast<double>(expected));
                CHECK(relErr < 1e-3, "lineX mid within tolerance");
            }
        }
    }

    {
        std::vector<float> line(nx);
        CHECK(reader.readLineX(ny - 1, nz - 1, line.data(), rcfg), "lineX last");
        CHECK(std::abs(line[nx - 1] - static_cast<float>(((nx-1)*ny + ny-1)*nz + nz-1))
              / std::abs(static_cast<float>(((nx-1)*ny + ny-1)*nz + nz-1)) < 1e-3,
              "lineX last value correct");
    }

    unlink(rawPath);
    unlink(rzfpPath);
}

static void testLineY() {
    const char* rawPath = "/tmp/test_line_y.raw";
    const char* rzfpPath = "/tmp/test_line_y.rzfp";

    const uint64_t nx = 33;
    const uint64_t ny = 25;
    const uint64_t nz = 41;
    writeRawFile(rawPath, nx, ny, nz);

    RzfpWriterConfig cfg;
    cfg.nx = nx; cfg.ny = ny; cfg.nz = nz;
    cfg.threads = 2;
    cfg.memory_limit_mb = 256;
    cfg.codec.error.policy = RelativeErrorPolicy::Strict;
    cfg.codec.error.contest_bound = 1e-3;
    cfg.codec.error.internal_bound = 7.5e-4;
    CHECK(writeRzfpFile(rawPath, rzfpPath, cfg), "write RZFP");

    RzfpReader reader(rzfpPath);
    CHECK(reader.ok(), "open reader");

    RzfpReaderConfig rcfg;
    rcfg.decode_threads = 2;

    {
        std::vector<float> line(ny);
        CHECK(reader.readLineY(0, 0, line.data(), rcfg), "lineY x=0,z=0");
        for (uint64_t y = 0; y < ny; ++y) {
            float expected = static_cast<float>((0 * ny + y) * nz + 0);
            if (expected == 0.0f) {
                CHECK(line[y] == 0.0f, "lineY zero preserved");
            } else {
                double relErr = std::abs(static_cast<double>(line[y] - expected))
                                / std::abs(static_cast<double>(expected));
                CHECK(relErr < 1e-3, "lineY non-zero within tolerance");
            }
        }
    }

    {
        std::vector<float> line(ny);
        CHECK(reader.readLineY(15, 30, line.data(), rcfg), "lineY x=15,z=30");
        CHECK(std::abs(line[ny-1] - static_cast<float>((15*ny + ny-1)*nz + 30))
              / std::abs(static_cast<float>((15*ny + ny-1)*nz + 30)) < 1e-3,
              "lineY last value correct");
    }

    unlink(rawPath);
    unlink(rzfpPath);
}

static void testLineZ() {
    const char* rawPath = "/tmp/test_line_z.raw";
    const char* rzfpPath = "/tmp/test_line_z.rzfp";

    const uint64_t nx = 33;
    const uint64_t ny = 25;
    const uint64_t nz = 41;
    writeRawFile(rawPath, nx, ny, nz);

    RzfpWriterConfig cfg;
    cfg.nx = nx; cfg.ny = ny; cfg.nz = nz;
    cfg.threads = 2;
    cfg.memory_limit_mb = 256;
    cfg.codec.error.policy = RelativeErrorPolicy::Strict;
    cfg.codec.error.contest_bound = 1e-3;
    cfg.codec.error.internal_bound = 7.5e-4;
    CHECK(writeRzfpFile(rawPath, rzfpPath, cfg), "write RZFP");

    RzfpReader reader(rzfpPath);
    CHECK(reader.ok(), "open reader");

    RzfpReaderConfig rcfg;
    rcfg.decode_threads = 2;

    {
        std::vector<float> line(nz);
        CHECK(reader.readLineZ(0, 0, line.data(), rcfg), "lineZ x=0,y=0");
        for (uint64_t z = 0; z < nz; ++z) {
            float expected = static_cast<float>((0 * ny + 0) * nz + z);
            if (expected == 0.0f) {
                CHECK(line[z] == 0.0f, "lineZ zero preserved");
            } else {
                double relErr = std::abs(static_cast<double>(line[z] - expected))
                                / std::abs(static_cast<double>(expected));
                CHECK(relErr < 1e-3, "lineZ non-zero within tolerance");
            }
        }
    }

    {
        std::vector<float> line(nz);
        CHECK(reader.readLineZ(20, 10, line.data(), rcfg), "lineZ x=20,y=10");
        CHECK(std::abs(line[nz-1] - static_cast<float>((20*ny + 10)*nz + nz-1))
              / std::abs(static_cast<float>((20*ny + 10)*nz + nz-1)) < 1e-3,
              "lineZ last value correct");
    }

    unlink(rawPath);
    unlink(rzfpPath);
}

static void testLineBounds() {
    const char* rawPath = "/tmp/test_line_bounds.raw";
    const char* rzfpPath = "/tmp/test_line_bounds.rzfp";

    const uint64_t nx = 20;
    const uint64_t ny = 20;
    const uint64_t nz = 20;
    writeRawFile(rawPath, nx, ny, nz);

    RzfpWriterConfig cfg;
    cfg.nx = nx; cfg.ny = ny; cfg.nz = nz;
    cfg.threads = 2;
    cfg.memory_limit_mb = 256;
    cfg.codec.error.policy = RelativeErrorPolicy::Strict;
    cfg.codec.error.contest_bound = 1e-3;
    cfg.codec.error.internal_bound = 7.5e-4;
    CHECK(writeRzfpFile(rawPath, rzfpPath, cfg), "write RZFP");

    RzfpReader reader(rzfpPath);
    RzfpReaderConfig rcfg;
    rcfg.decode_threads = 2;

    std::vector<float> buf(100);
    CHECK(!reader.readLineX(ny, 0, buf.data(), rcfg), "lineX out of y bounds");
    CHECK(!reader.readLineX(0, nz, buf.data(), rcfg), "lineX out of z bounds");
    CHECK(!reader.readLineY(nx, 0, buf.data(), rcfg), "lineY out of x bounds");
    CHECK(!reader.readLineY(0, nz, buf.data(), rcfg), "lineY out of z bounds");
    CHECK(!reader.readLineZ(nx, 0, buf.data(), rcfg), "lineZ out of x bounds");
    CHECK(!reader.readLineZ(0, ny, buf.data(), rcfg), "lineZ out of y bounds");

    CHECK(!reader.readLineX(0, 0, nullptr, rcfg), "lineX null output");

    unlink(rawPath);
    unlink(rzfpPath);
}

int main() {
    testLineX();
    testLineY();
    testLineZ();
    testLineBounds();

    std::cout << "Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed > 0 ? 1 : 0;
}
