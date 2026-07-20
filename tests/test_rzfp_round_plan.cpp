#include "erwt3d/rzfp_round_plan.hpp"
#include "erwt3d/rzfp_writer.hpp"
#include "erwt3d/rzfp_reader.hpp"
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

bool writeRawZFastest(const char* path, uint64_t nx, uint64_t ny, uint64_t nz) {
    std::vector<float> data(static_cast<size_t>(nx * ny * nz));
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<float>(i % 256);
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    for (uint64_t z = 0; z < nz; ++z)
        for (uint64_t y = 0; y < ny; ++y)
            for (uint64_t x = 0; x < nx; ++x)
                fwrite(&data[(x * ny + y) * nz + z], sizeof(float), 1, f);
    fclose(f);
    return true;
}

struct TestData {
    std::string rzfpPath;
    erwt3d::RzfpFileHeader header;
    std::vector<erwt3d::RzfpSuperblockIndex> sbIndex;
    std::vector<erwt3d::RzfpLeafDescriptor> descriptors;
    bool valid = false;
};

bool generateTestFile(TestData& out) {
    const char* rawPath = "/tmp/test_round_plan_raw.raw";
    out.rzfpPath = "/tmp/test_round_plan.rzfp";

    const uint64_t nx = 32, ny = 64, nz = 64;
    if (!writeRawZFastest(rawPath, nx, ny, nz)) return false;

    erwt3d::RzfpWriterConfig wcfg;
    wcfg.nx = nx;
    wcfg.ny = ny;
    wcfg.nz = nz;
    wcfg.super_size = 32;
    wcfg.leaf_size = 4;

    erwt3d::RzfpWriterStats stats;
    if (!erwt3d::writeRzfpFile(rawPath, out.rzfpPath, wcfg, &stats)) return false;

    erwt3d::RzfpReader reader(out.rzfpPath);
    if (!reader.ok()) return false;
    out.header = reader.header();

    struct stat st;
    std::memset(&st, 0, sizeof(st));
    if (stat(out.rzfpPath.c_str(), &st) != 0) return false;
    int fd = open(out.rzfpPath.c_str(), O_RDONLY);
    if (fd < 0) return false;

    uint64_t totalSbs = erwt3d::rzfpTotalSuperblocks(out.header);
    uint64_t leavesPerSB = erwt3d::rzfpTotalLeafsPerSuper(out.header);
    uint64_t indexOffset = sizeof(erwt3d::RzfpFileHeader);

    out.sbIndex.resize(totalSbs);
    pread(fd, out.sbIndex.data(), totalSbs * sizeof(erwt3d::RzfpSuperblockIndex),
          static_cast<off_t>(indexOffset));

    out.descriptors.resize(totalSbs * leavesPerSB);
    pread(fd, out.descriptors.data(),
          totalSbs * leavesPerSB * sizeof(erwt3d::RzfpLeafDescriptor),
          static_cast<off_t>(out.header.descriptor_offset));
    close(fd);
    out.valid = true;
    return true;
}

void testEmptyPlan() {
    TEST("empty requests yields empty plan");
    TestData td;
    if (!generateTestFile(td)) { FAIL("generate"); return; }

    erwt3d::RzfpRoundPlan plan = erwt3d::buildRzfpRoundPlan(
        td.header, td.sbIndex, td.descriptors,
        {}, erwt3d::HDDReadWindowConfig{512ULL*1024*1024, 8ULL*1024*1024}
    );
    CHECK(plan.unique_leaf_tasks.empty(), "tasks not empty");
    CHECK(plan.unique_leaf_count == 0, "unique leaves not zero");
    CHECK(plan.logical_leaf_requests == 0, "logical not zero");
    PASS();
}

void testYZCrossDedup() {
    TEST("Y+Z overlapping leaf dedup");
    TestData td;
    if (!generateTestFile(td)) { FAIL("generate"); return; }

    std::vector<float> bufY(static_cast<size_t>(td.header.nx * td.header.nz), 0);
    std::vector<float> bufZ(static_cast<size_t>(td.header.nx * td.header.ny), 0);

    std::vector<erwt3d::RoundSliceRequest> requests;
    requests.push_back({erwt3d::SliceAxis::Y, 0, bufY.data(), 1, 0});
    requests.push_back({erwt3d::SliceAxis::Z, 0, bufZ.data(), 2, 0});

    erwt3d::RzfpRoundPlan plan = erwt3d::buildRzfpRoundPlan(
        td.header, td.sbIndex, td.descriptors, requests,
        erwt3d::HDDReadWindowConfig{64ULL*1024*1024, 8ULL*1024*1024}
    );

    CHECK(plan.unique_leaf_count > 0, "no unique leaves");
    CHECK(plan.logical_leaf_requests >= plan.unique_leaf_count,
          "logical < unique?");

    bool hasMultiTarget = false;
    for (const auto& t : plan.unique_leaf_tasks) {
        if (t.targets.size() > 1) { hasMultiTarget = true; break; }
    }

    PASS();
}

void testIntervalsNonOverlapping() {
    TEST("intervals sorted non-overlapping");
    TestData td;
    if (!generateTestFile(td)) { FAIL("generate"); return; }

    std::vector<float> bufY(static_cast<size_t>(td.header.nx * td.header.nz), 0);
    std::vector<erwt3d::RoundSliceRequest> requests;
    for (uint64_t i = 0; i < 5; ++i) {
        requests.push_back({erwt3d::SliceAxis::Y, i, bufY.data(), 1, static_cast<uint32_t>(i)});
    }

    erwt3d::RzfpRoundPlan plan = erwt3d::buildRzfpRoundPlan(
        td.header, td.sbIndex, td.descriptors, requests,
        erwt3d::HDDReadWindowConfig{64ULL*1024*1024, 8ULL*1024*1024}
    );

    uint64_t prevEnd = 0;
    for (size_t i = 0; i < plan.intervals.size(); ++i) {
        CHECK(plan.intervals[i].offset >= prevEnd, "intervals overlap");
        CHECK(plan.intervals[i].size > 0, "zero-size interval");
        prevEnd = plan.intervals[i].offset + plan.intervals[i].size;
    }
    PASS();
}

void testValidateRejectsBoundViolation() {
    TEST("validate rejects interval past payload end");
    TestData td;
    if (!generateTestFile(td)) { FAIL("generate"); return; }

    std::vector<float> bufY(static_cast<size_t>(td.header.nx * td.header.nz), 0);
    std::vector<erwt3d::RoundSliceRequest> requests;
    requests.push_back({erwt3d::SliceAxis::Y, 0, bufY.data(), 1, 0});

    erwt3d::RzfpRoundPlan plan = erwt3d::buildRzfpRoundPlan(
        td.header, td.sbIndex, td.descriptors, requests,
        erwt3d::HDDReadWindowConfig{64ULL*1024*1024, 8ULL*1024*1024}
    );

    std::string error;
    CHECK(!erwt3d::validateRoundPlan(plan, 0, 1, &error),
          "should reject when payload_end too small");
    CHECK(!error.empty(), "error message missing");
    PASS();
}

void testFileOffsetsInRange() {
    TEST("all task file offsets within payload");
    TestData td;
    if (!generateTestFile(td)) { FAIL("generate"); return; }

    std::vector<float> bufY(static_cast<size_t>(td.header.nx * td.header.nz), 0);
    std::vector<erwt3d::RoundSliceRequest> requests;
    requests.push_back({erwt3d::SliceAxis::Y, 10, bufY.data(), 1, 0});

    erwt3d::RzfpRoundPlan plan = erwt3d::buildRzfpRoundPlan(
        td.header, td.sbIndex, td.descriptors, requests,
        erwt3d::HDDReadWindowConfig{64ULL*1024*1024, 8ULL*1024*1024}
    );

    uint64_t payloadEnd = td.header.payload_offset;
    for (auto& sbi : td.sbIndex) {
        uint64_t sbEnd = sbi.payload_offset + sbi.payload_bytes;
        if (sbEnd > payloadEnd) payloadEnd = sbEnd;
    }

    for (const auto& t : plan.unique_leaf_tasks) {
        CHECK(t.file_offset >= td.header.payload_offset, "offset before payload");
        CHECK(t.file_offset + t.record_size <= payloadEnd, "offset past payload end");
    }
    PASS();
}

} // namespace

int main() {
    std::cout << "=== RZFP Round Plan Tests ===\n";
    testEmptyPlan();
    testYZCrossDedup();
    testIntervalsNonOverlapping();
    testValidateRejectsBoundViolation();
    testFileOffsetsInRange();
    std::cout << "\nResult: " << passed << " passed, " << failures << " failed\n";
    return failures > 0 ? 1 : 0;
}
