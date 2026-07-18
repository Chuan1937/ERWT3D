#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/rzfp_writer.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;
int passed = 0;

void TEST(const char* name) { std::cout << "  " << name << "..." << std::flush; }
void FAIL(const char* msg) { std::cout << " FAIL: " << msg << "\n"; ++failures; }
void PASS() { std::cout << " PASS\n"; ++passed; }
void CHECK(bool cond, const char* msg) { if (!cond) { FAIL(msg); return; } }

void writeRawZFastest(const char* path, uint64_t nx, uint64_t ny, uint64_t nz,
                      std::vector<float>& data) {
    data.resize(static_cast<size_t>(nx * ny * nz));
    for (size_t z = 0; z < nz; ++z)
        for (size_t y = 0; y < ny; ++y)
            for (size_t x = 0; x < nx; ++x)
                data[(x * ny + y) * nz + z] =
                    static_cast<float>((x * 100 + y * 10 + z) % 997) / 100.0f;
    FILE* f = fopen(path, "wb");
    for (uint64_t z = 0; z < nz; ++z)
        for (uint64_t y = 0; y < ny; ++y)
            for (uint64_t x = 0; x < nx; ++x)
                fwrite(&data[(x * ny + y) * nz + z], sizeof(float), 1, f);
    fclose(f);
}

bool generateTestFile(const char* rzfpPath, const char* rawPath,
                      uint64_t nx, uint64_t ny, uint64_t nz,
                      std::vector<float>& data) {
    writeRawZFastest(rawPath, nx, ny, nz, data);

    erwt3d::RzfpWriterConfig wcfg;
    wcfg.nx = nx;
    wcfg.ny = ny;
    wcfg.nz = nz;
    wcfg.super_size = 32;
    wcfg.leaf_size = 4;

    erwt3d::RzfpWriterStats stats;
    if (!erwt3d::writeRzfpFile(rawPath, rzfpPath, wcfg, &stats)) return false;
    CHECK(stats.violation_count == 0, "relative error violations");
    return true;
}

void testP4vsP5OutputMatch() {
    TEST("P4 groups vs P5 round output match");
    const uint64_t nx = 32, ny = 64, nz = 64;
    const char* rawPath = "/tmp/test_round_reader_raw.raw";
    const char* rzfpPath = "/tmp/test_round_reader.rzfp";

    std::vector<float> refData;
    if (!generateTestFile(rzfpPath, rawPath, nx, ny, nz, refData)) {
        FAIL("generate"); return;
    }

    std::vector<float> p4_xr(static_cast<size_t>(ny * nz), 0);
    std::vector<float> p4_yr(static_cast<size_t>(nx * nz), 0);
    std::vector<float> p4_zr(static_cast<size_t>(nx * ny), 0);
    std::vector<float> p4_xc(static_cast<size_t>(ny * nz), 0);
    std::vector<float> p4_yc(static_cast<size_t>(nx * nz), 0);
    std::vector<float> p4_zc(static_cast<size_t>(nx * ny), 0);

    std::vector<float> p5_xr(static_cast<size_t>(ny * nz), 0);
    std::vector<float> p5_yr(static_cast<size_t>(nx * nz), 0);
    std::vector<float> p5_zr(static_cast<size_t>(nx * ny), 0);
    std::vector<float> p5_xc(static_cast<size_t>(ny * nz), 0);
    std::vector<float> p5_yc(static_cast<size_t>(nx * nz), 0);
    std::vector<float> p5_zc(static_cast<size_t>(nx * ny), 0);

    erwt3d::RzfpReader reader(rzfpPath);
    CHECK(reader.ok(), "open failed");
    (void)reader.ensureDeviceProfile();

    erwt3d::RzfpReaderConfig cfg;
    cfg.strategy = erwt3d::RzfpReadStrategy::Auto;
    cfg.decode_threads = 1;
    cfg.adaptive.auto_calibrate_device = true;
    cfg.adaptive.cache_policy = erwt3d::CachePolicy::StableAuto;
    cfg.hdd.read_window_bytes = 64ULL * 1024 * 1024;
    cfg.hdd.max_gap_bytes = 8ULL * 1024 * 1024;

    uint64_t xr_idx = 10, yr_idx = 20, zr_idx = 30;
    uint64_t xc_idx = 15, yc_idx = 30, zc_idx = 25;

    reader.readSlice(erwt3d::SliceAxis::X, xr_idx, p4_xr.data(), 1, 4096, cfg.hdd);
    reader.readSlice(erwt3d::SliceAxis::Y, yr_idx, p4_yr.data(), 1, 4096, cfg.hdd);
    reader.readSlice(erwt3d::SliceAxis::Z, zr_idx, p4_zr.data(), 1, 4096, cfg.hdd);
    reader.readSlice(erwt3d::SliceAxis::X, xc_idx, p4_xc.data(), 1, 4096, cfg.hdd);
    reader.readSlice(erwt3d::SliceAxis::Y, yc_idx, p4_yc.data(), 1, 4096, cfg.hdd);
    reader.readSlice(erwt3d::SliceAxis::Z, zc_idx, p4_zc.data(), 1, 4096, cfg.hdd);

    std::vector<erwt3d::RzfpReader::ContestRoundGroup> groups;
    {
        erwt3d::RzfpReader::ContestRoundGroup g;
        g.axis = erwt3d::SliceAxis::X; g.name = "x_random";
        g.indices = {xr_idx}; g.outputs = {p5_xr.data()};
        groups.push_back(g);
    }
    {
        erwt3d::RzfpReader::ContestRoundGroup g;
        g.axis = erwt3d::SliceAxis::Y; g.name = "y_random";
        g.indices = {yr_idx}; g.outputs = {p5_yr.data()};
        groups.push_back(g);
    }
    {
        erwt3d::RzfpReader::ContestRoundGroup g;
        g.axis = erwt3d::SliceAxis::Z; g.name = "z_random";
        g.indices = {zr_idx}; g.outputs = {p5_zr.data()};
        groups.push_back(g);
    }
    {
        erwt3d::RzfpReader::ContestRoundGroup g;
        g.axis = erwt3d::SliceAxis::X; g.name = "x_continuous";
        g.indices = {xc_idx}; g.outputs = {p5_xc.data()};
        groups.push_back(g);
    }
    {
        erwt3d::RzfpReader::ContestRoundGroup g;
        g.axis = erwt3d::SliceAxis::Y; g.name = "y_continuous";
        g.indices = {yc_idx}; g.outputs = {p5_yc.data()};
        groups.push_back(g);
    }
    {
        erwt3d::RzfpReader::ContestRoundGroup g;
        g.axis = erwt3d::SliceAxis::Z; g.name = "z_continuous";
        g.indices = {zc_idx}; g.outputs = {p5_zc.data()};
        groups.push_back(g);
    }

    std::vector<erwt3d::RzfpReader::RzfpRoundReadResult> roundResults;
    CHECK(reader.readContestRound(groups, cfg, &roundResults), "readContestRound failed");
    CHECK(roundResults.size() == 6, "wrong result count");

    auto vecMatch = [](const std::vector<float>& a, const std::vector<float>& b,
                        const char* label) -> bool {
        if (a.size() != b.size()) { std::cout << "size mismatch " << label; return false; }
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::fabs(a[i] - b[i]) > 1e-5f) {
                std::cout << "value mismatch " << label << "[" << i << "]";
                return false;
            }
        }
        return true;
    };

    CHECK(vecMatch(p4_xr, p5_xr, "x_random"), "x_random mismatch");
    CHECK(vecMatch(p4_yr, p5_yr, "y_random"), "y_random mismatch");
    CHECK(vecMatch(p4_zr, p5_zr, "z_random"), "z_random mismatch");
    CHECK(vecMatch(p4_xc, p5_xc, "x_continuous"), "x_continuous mismatch");
    CHECK(vecMatch(p4_yc, p5_yc, "y_continuous"), "y_continuous mismatch");
    CHECK(vecMatch(p4_zc, p5_zc, "z_continuous"), "z_continuous mismatch");

    PASS();
}

void testRoundPlanStatsBuilt() {
    TEST("round plan statistics populated");
    const uint64_t nx = 32, ny = 64, nz = 64;
    const char* rawPath = "/tmp/test_round_reader_stats_raw.raw";
    const char* rzfpPath = "/tmp/test_round_reader_stats.rzfp";

    std::vector<float> refData;
    if (!generateTestFile(rzfpPath, rawPath, nx, ny, nz, refData)) {
        FAIL("generate"); return;
    }

    erwt3d::RzfpReader reader(rzfpPath);
    CHECK(reader.ok(), "open failed");

    std::vector<float> bufYr(nx * nz * 2, 0), bufYc(nx * nz, 0);
    std::vector<float> bufZr(nx * ny * 2, 0), bufZc(nx * ny, 0);

    std::vector<erwt3d::RzfpReader::ContestRoundGroup> groups;
    {
        erwt3d::RzfpReader::ContestRoundGroup g;
        g.axis = erwt3d::SliceAxis::Y; g.name = "y_random";
        g.indices = {10, 20}; g.outputs = {bufYr.data(), bufYr.data() + (nx * nz)};
        groups.push_back(g);
    }
    {
        erwt3d::RzfpReader::ContestRoundGroup g;
        g.axis = erwt3d::SliceAxis::Z; g.name = "z_random";
        g.indices = {10, 20}; g.outputs = {bufZr.data(), bufZr.data() + (nx * ny)};
        groups.push_back(g);
    }
    {
        erwt3d::RzfpReader::ContestRoundGroup g;
        g.axis = erwt3d::SliceAxis::Y; g.name = "y_cont";
        g.indices = {30}; g.outputs = {bufYc.data()};
        groups.push_back(g);
    }
    {
        erwt3d::RzfpReader::ContestRoundGroup g;
        g.axis = erwt3d::SliceAxis::Z; g.name = "z_cont";
        g.indices = {30}; g.outputs = {bufZc.data()};
        groups.push_back(g);
    }

    erwt3d::RzfpReaderConfig cfg;
    cfg.strategy = erwt3d::RzfpReadStrategy::Auto;
    cfg.decode_threads = 1;
    cfg.hdd.read_window_bytes = 64ULL * 1024 * 1024;

    std::vector<erwt3d::RzfpReader::RzfpRoundReadResult> results;
    CHECK(reader.readContestRound(groups, cfg, &results), "read failed");

    bool hasPlan = false;
    for (const auto& r : results) {
        if (r.round_plan_built) { hasPlan = true; break; }
    }
    CHECK(hasPlan, "no round plan built for Y/Z groups");

    for (const auto& r : results) {
        if (!r.round_plan_built) continue;
        CHECK(r.unique_leaves > 0, "unique_leaves is 0");
        CHECK(r.logical_leaf_requests > 0, "logical_leaf_requests is 0");
        CHECK(r.round_unique_superblocks > 0, "unique_superblocks is 0");
    }

    PASS();
}

} // namespace

int main() {
    std::cout << "=== RZFP Round Reader Tests ===\n";
    testP4vsP5OutputMatch();
    testRoundPlanStatsBuilt();
    std::cout << "\nResult: " << passed << " passed, " << failures << " failed\n";
    return failures > 0 ? 1 : 0;
}
