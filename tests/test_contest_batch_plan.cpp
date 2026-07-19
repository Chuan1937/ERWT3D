#include "erwt3d/contest_round_executor.hpp"
#include "erwt3d/rzfp_writer.hpp"
#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/memory_budget.hpp"

#include <cmath>
#include <cstring>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0, passed = 0;
void TEST(const char* n) { std::cout << "  " << n << "..." << std::flush; }
void FAIL(const char* m) { std::cout << " FAIL: " << m << "\n"; ++failures; }
void PASS() { std::cout << " PASS\n"; ++passed; }
void CHECK(bool c, const char* m) { if (!c) { FAIL(m); return; } }

void writeRawZ(const char* p, uint64_t nx, uint64_t ny, uint64_t nz,
               std::vector<float>& data) {
    data.resize(static_cast<size_t>(nx * ny * nz));
    for (size_t z = 0; z < nz; ++z)
        for (size_t y = 0; y < ny; ++y)
            for (size_t x = 0; x < nx; ++x)
                data[(x * ny + y) * nz + z] = static_cast<float>((x*100+y*10+z)%31)/100.0f;
    FILE* f = fopen(p, "wb");
    for (uint64_t z = 0; z < nz; ++z)
        for (uint64_t y = 0; y < ny; ++y)
            for (uint64_t x = 0; x < nx; ++x)
                fwrite(&data[(x*ny+y)*nz+z], sizeof(float), 1, f);
    fclose(f);
}

bool createRzfp(const char* raw, const char* rzfp,
                uint64_t nx, uint64_t ny, uint64_t nz) {
    erwt3d::RzfpWriterConfig c;
    c.nx = nx; c.ny = ny; c.nz = nz;
    c.super_size = 32; c.leaf_size = 4;
    return erwt3d::writeRzfpFile(raw, rzfp, c, nullptr);
}

void testUnequalGroupLengths() {
    TEST("unequal group lengths with forced multi-batch");

    const uint64_t nx = 32, ny = 64, nz = 64;
    const char* raw = "/tmp/test_batch_unequal.raw";
    const char* rzfp = "/tmp/test_batch_unequal.rzfp";

    std::vector<float> d;
    writeRawZ(raw, nx, ny, nz, d);
    CHECK(createRzfp(raw, rzfp, nx, ny, nz), "create rzfp");

    erwt3d::RzfpReader reader(rzfp);
    CHECK(reader.ok(), "open");
    const auto& hdr = reader.header();

    std::vector<erwt3d::ContestExecutionGroup> groups;
    std::vector<uint64_t> xr = {0,1,2,3,4,5,6};
    std::vector<uint64_t> yr = {10,20,30,40,50};
    std::vector<uint64_t> zr = {5,15,25};
    std::vector<uint64_t> xc = {8};
    std::vector<uint64_t> yc = {35};
    std::vector<uint64_t> zc = {45};

    groups.push_back({erwt3d::SliceAxis::X, "x_random", &xr});
    groups.push_back({erwt3d::SliceAxis::Y, "y_random", &yr});
    groups.push_back({erwt3d::SliceAxis::Z, "z_random", &zr});
    groups.push_back({erwt3d::SliceAxis::X, "x_continuous", &xc});
    groups.push_back({erwt3d::SliceAxis::Y, "y_continuous", &yc});
    groups.push_back({erwt3d::SliceAxis::Z, "z_continuous", &zc});

    uint64_t maxOut = 0;
    for (const auto& g : groups) {
        uint64_t e = 0;
        switch (g.axis) {
            case erwt3d::SliceAxis::X: e = ny*nz; break;
            case erwt3d::SliceAxis::Y: e = nx*nz; break;
            case erwt3d::SliceAxis::Z: e = nx*ny; break;
        }
        maxOut = std::max(maxOut, e * sizeof(float) * g.indices->size());
    }

    erwt3d::MemoryBudget budget = erwt3d::makeMemoryBudget(
        "2", reader.payloadBytes(), maxOut, 7);

    erwt3d::RzfpReaderConfig cfg;
    cfg.strategy = erwt3d::RzfpReadStrategy::Auto;
    cfg.decode_threads = 1;
    cfg.hdd.read_window_bytes = 8ULL*1024*1024;

    erwt3d::ContestExecutionProfile prof;
    bool ok = erwt3d::executeContestRound(
        reader, hdr, groups, "/tmp", "test_batch",
        cfg, budget, &prof);

    CHECK(ok, "execution failed");

    for (const auto& g : groups) {
        for (size_t i = 0; i < g.indices->size(); ++i) {
            std::ostringstream p;
            p << "/tmp/test_batch_g";
            size_t gidx = &g - groups.data();
            p << gidx << "_" << g.name << "_" << i << ".raw";
            struct stat st;
            CHECK(stat(p.str().c_str(), &st) == 0, "output file missing");
            uint64_t expectedSize = 0;
            switch (g.axis) {
                case erwt3d::SliceAxis::X: expectedSize = ny*nz*sizeof(float); break;
                case erwt3d::SliceAxis::Y: expectedSize = nx*nz*sizeof(float); break;
                case erwt3d::SliceAxis::Z: expectedSize = nx*ny*sizeof(float); break;
            }
            CHECK(static_cast<uint64_t>(st.st_size) == expectedSize, "wrong output size");
        }
    }

    CHECK(prof.phase_count >= 1, "no phases");
    CHECK(prof.read_time_ms >= 0, "negative time");
    PASS();
}

void testLowMemoryVsHighMemory() {
    TEST("low memory output matches high memory");

    const uint64_t nx = 32, ny = 64, nz = 64;
    const char* raw = "/tmp/test_batch_compare.raw";
    const char* rzfp = "/tmp/test_batch_compare.rzfp";

    std::vector<float> d;
    writeRawZ(raw, nx, ny, nz, d);
    CHECK(createRzfp(raw, rzfp, nx, ny, nz), "create rzfp");

    std::vector<uint64_t> xr = {2,5,7};
    std::vector<uint64_t> yr = {10,20,30};
    std::vector<uint64_t> zr = {5,25};
    std::vector<uint64_t> xc = {8};
    std::vector<uint64_t> yc = {35};
    std::vector<uint64_t> zc = {45};

    auto readOutput = [&](const char* tag, erwt3d::MemoryBudget& budget) -> std::string {
        erwt3d::RzfpReader reader(rzfp);
        const auto& hdr = reader.header();
        std::vector<erwt3d::ContestExecutionGroup> grps;
        grps.push_back({erwt3d::SliceAxis::X, "x_random", &xr});
        grps.push_back({erwt3d::SliceAxis::Y, "y_random", &yr});
        grps.push_back({erwt3d::SliceAxis::Z, "z_random", &zr});
        grps.push_back({erwt3d::SliceAxis::X, "x_continuous", &xc});
        grps.push_back({erwt3d::SliceAxis::Y, "y_continuous", &yc});
        grps.push_back({erwt3d::SliceAxis::Z, "z_continuous", &zc});

        erwt3d::RzfpReaderConfig cfg;
        cfg.strategy = erwt3d::RzfpReadStrategy::Auto;
        cfg.decode_threads = 1;
        cfg.hdd.read_window_bytes = 8ULL*1024*1024;

        std::string outDir = "/tmp/" + std::string(tag);
        mkdir(outDir.c_str(), 0755);
        erwt3d::executeContestRound(reader, hdr, grps, outDir, "cmp", cfg, budget, nullptr);

        std::string hash;
        for (size_t gi = 0; gi < grps.size(); ++gi) {
            for (size_t i = 0; i < grps[gi].indices->size(); ++i) {
                std::ostringstream p;
                p << outDir << "/cmp_g" << gi << "_" << grps[gi].name << "_" << i << ".raw";
                struct stat st;
                stat(p.str().c_str(), &st);
                hash += std::to_string(gi) + ":" + std::to_string(st.st_size) + ";";
            }
        }
        return hash;
    };

    uint64_t maxOut = uint64_t(nx*ny*sizeof(float)*3);
    erwt3d::MemoryBudget big = erwt3d::makeMemoryBudget("auto", 1000000, maxOut, 7);
    erwt3d::MemoryBudget small = erwt3d::makeMemoryBudget("1", 1000000, maxOut, 7);

    auto hashBig = readOutput("batch_big", big);
    auto hashSmall = readOutput("batch_small", small);

    CHECK(hashBig == hashSmall, "output differs between mem modes");
    PASS();
}

void testForcedMultiBatch() {
    TEST("forced multi-batch with contest-like group structure");

    const uint64_t nx = 32, ny = 64, nz = 64;
    const char* raw = "/tmp/test_batch_multi.raw";
    const char* rzfp = "/tmp/test_batch_multi.rzfp";

    std::vector<float> d;
    writeRawZ(raw, nx, ny, nz, d);
    CHECK(createRzfp(raw, rzfp, nx, ny, nz), "create rzfp");

    erwt3d::RzfpReader reader(rzfp);
    CHECK(reader.ok(), "open");
    const auto& hdr = reader.header();

    auto makeIndices = [](int count) -> std::vector<uint64_t> {
        std::vector<uint64_t> v(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) v[i] = static_cast<uint64_t>(i * 2);
        return v;
    };

    auto xr = makeIndices(6);
    auto yr = makeIndices(5);
    auto zr = makeIndices(4);
    auto xc = makeIndices(3);
    auto yc = makeIndices(2);
    auto zc = makeIndices(1);

    std::vector<erwt3d::ContestExecutionGroup> grps = {
        {erwt3d::SliceAxis::X, "x_random", &xr},
        {erwt3d::SliceAxis::Y, "y_random", &yr},
        {erwt3d::SliceAxis::Z, "z_random", &zr},
        {erwt3d::SliceAxis::X, "x_continuous", &xc},
        {erwt3d::SliceAxis::Y, "y_continuous", &yc},
        {erwt3d::SliceAxis::Z, "z_continuous", &zc},
    };

    const uint64_t elemY = nx * nz * sizeof(float);
    const uint64_t maxOutBytes = std::max<uint64_t>(elemY * 6, 1024);
    erwt3d::MemoryBudget budget = erwt3d::makeMemoryBudget(
        "1", reader.payloadBytes(), maxOutBytes, 7);

    erwt3d::RzfpReaderConfig cfg;
    cfg.strategy = erwt3d::RzfpReadStrategy::Auto;
    cfg.decode_threads = 1;
    cfg.hdd.read_window_bytes = 8ULL*1024*1024;

    erwt3d::ContestExecutionProfile prof;
    std::string outDir = "/tmp/test_batch_multi_out";
    mkdir(outDir.c_str(), 0755);
    bool ok = erwt3d::executeContestRound(
        reader, hdr, grps, outDir, "multi", cfg, budget, &prof);

    CHECK(ok, "execution failed");
    CHECK(prof.phase_count > 0, "zero phases");
    CHECK(prof.read_time_ms >= 0, "negative time");
    CHECK(prof.peak_accounted_bytes >= 0, "peak_accounted_bytes not set");
    PASS();
}

} // namespace

int main() {
    std::cout << "=== Contest Batch Plan Tests ===\n";
    testUnequalGroupLengths();
    testLowMemoryVsHighMemory();
    testForcedMultiBatch();
    std::cout << "\nResult: " << passed << " passed, " << failures << " failed\n";
    return failures > 0 ? 1 : 0;
}
