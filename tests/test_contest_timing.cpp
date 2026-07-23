#include "erwt3d/contest_groups.hpp"
#include "erwt3d/contest_positions.hpp"
#include <cassert>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/stat.h>
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

static bool mkdirOne(const std::string& path) {
    if (path.empty() || path == ".") return true;
    if (mkdir(path.c_str(), 0755) == 0) return true;
    return errno == EEXIST;
}

static void testTimingFormula() {
    ContestPositions positions;
    positions.x_random.resize(1, 0);
    positions.y_random.resize(1, 0);
    positions.z_random.resize(1, 0);
    positions.x_continuous.resize(1, 0);
    positions.y_continuous.resize(1, 0);
    positions.z_continuous.resize(1, 0);

    ContestReadBatchFunction reader = [](
        SliceAxis axis,
        const std::vector<uint64_t>& indices,
        std::vector<std::vector<float>>& outputs
    ) -> bool {
        for (auto& o : outputs) std::fill(o.begin(), o.end(), 0.0f);
        return true;
    };

    mkdirOne("/tmp/test_timing_out");
    ContestUnifiedProfile profile;
    bool ok = executeContestGroups(positions, "/tmp/test_timing_out", 10, 10, 10, reader, &profile);
    CHECK(ok, "executeContestGroups should succeed");

    double expected = (profile.x_random.time_ms + profile.y_random.time_ms +
                       profile.z_random.time_ms + profile.x_continuous.time_ms +
                       profile.y_continuous.time_ms + profile.z_continuous.time_ms) / 6.0;
    CHECK(std::abs(profile.t_composite_ms - expected) < 0.01, "T_composite = sum/6");

    double expectedTX = (profile.x_random.time_ms + profile.x_continuous.time_ms) / 2.0;
    double expectedTY = (profile.y_random.time_ms + profile.y_continuous.time_ms) / 2.0;
    double expectedTZ = (profile.z_random.time_ms + profile.z_continuous.time_ms) / 2.0;
    CHECK(std::abs(profile.t_x_ms - expectedTX) < 0.01, "T_X = (xr+xc)/2");
    CHECK(std::abs(profile.t_y_ms - expectedTY) < 0.01, "T_Y = (yr+yc)/2");
    CHECK(std::abs(profile.t_z_ms - expectedTZ) < 0.01, "T_Z = (zr+zc)/2");

    double expectedComposite = (expectedTX + expectedTY + expectedTZ) / 3.0;
    CHECK(std::abs(profile.t_composite_ms - expectedComposite) < 0.01, "T_composite = (TX+TY+TZ)/3");

    CHECK(profile.output_file_count == 6, "output file count = 6");
}

static void testAllTimesPositive() {
    ContestPositions positions;
    positions.x_random.resize(5, 0);
    positions.y_random.resize(5, 0);
    positions.z_random.resize(5, 0);
    positions.x_continuous.resize(2, 0);
    positions.y_continuous.resize(2, 0);
    positions.z_continuous.resize(2, 0);

    ContestReadBatchFunction reader = [](
        SliceAxis axis,
        const std::vector<uint64_t>& indices,
        std::vector<std::vector<float>>& outputs
    ) -> bool {
        volatile float sink = 0.0f;
        for (auto& o : outputs)
            for (auto& v : o) { v = 1.0f; sink += v; }
        return true;
    };

    mkdirOne("/tmp/test_timing_pos");
    ContestUnifiedProfile profile;
    bool ok = executeContestGroups(positions, "/tmp/test_timing_pos", 10, 10, 10, reader, &profile);
    CHECK(ok, "execute ok");

    CHECK(profile.x_random.time_ms >= 0, "x_random time >= 0");
    CHECK(profile.y_random.time_ms >= 0, "y_random time >= 0");
    CHECK(profile.z_random.time_ms >= 0, "z_random time >= 0");
    CHECK(profile.x_continuous.time_ms >= 0, "x_continuous time >= 0");
    CHECK(profile.y_continuous.time_ms >= 0, "y_continuous time >= 0");
    CHECK(profile.z_continuous.time_ms >= 0, "z_continuous time >= 0");
    CHECK(profile.t_composite_ms >= 0, "t_composite >= 0");
}

static void testReadFailurePropagates() {
    ContestPositions positions;
    positions.x_random.resize(1, 0);
    positions.y_random.resize(0);
    positions.z_random.resize(0);
    positions.x_continuous.resize(0);
    positions.y_continuous.resize(0);
    positions.z_continuous.resize(0);

    ContestReadBatchFunction reader = [](
        SliceAxis, const std::vector<uint64_t>&,
        std::vector<std::vector<float>>&
    ) -> bool {
        return false;
    };

    mkdirOne("/tmp/test_timing_fail");
    ContestUnifiedProfile profile;
    bool ok = executeContestGroups(positions, "/tmp/test_timing_fail", 10, 10, 10, reader, &profile);
    CHECK(!ok, "read failure should propagate");
}

int main() {
    testTimingFormula();
    testAllTimesPositive();
    testReadFailurePropagates();

    std::cout << "Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed > 0 ? 1 : 0;
}
