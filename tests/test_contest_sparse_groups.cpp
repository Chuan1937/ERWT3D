#include "erwt3d/contest_groups.hpp"
#include "erwt3d/contest_positions.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

using namespace erwt3d;

namespace {

int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " at line " << __LINE__ << "\n"; \
        ++failures; \
    } \
} while (0)

std::string tempFile(const char* stem) {
    return std::string("/tmp/") + stem + "_" + std::to_string(getpid()) + ".csv";
}

std::string tempDir(const char* stem) {
    const std::string path = std::string("/tmp/") + stem + "_" + std::to_string(getpid());
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

void writeRandom(std::ofstream& out, char axis, int count) {
    for (int i = 0; i < count; ++i) {
        out << axis << ",random," << i << "\n";
    }
}

void writeContinuous(std::ofstream& out, char axis, int start, int count) {
    for (int i = 0; i < count; ++i) {
        out << axis << ",continuous," << start + i << "\n";
    }
}

void testXOnlyCsv() {
    const std::string path = tempFile("contest_x_only");
    {
        std::ofstream out(path);
        out << "axis,type,index\n";
        writeRandom(out, 'x', 100);
        writeContinuous(out, 'x', 200, 10);
    }

    ContestPositions pos;
    std::string error;
    CHECK(parsePositionsFile(path, 1000, 1000, 1000, 100, 10, pos, error), error);
    CHECK(pos.x_random.size() == 100, "X-only random count");
    CHECK(pos.x_continuous.size() == 10, "X-only continuous count");
    CHECK(pos.y_random.empty() && pos.y_continuous.empty(), "Y groups remain empty");
    CHECK(pos.z_random.empty() && pos.z_continuous.empty(), "Z groups remain empty");
    std::filesystem::remove(path);
}

void testYRandomOnlyCsv() {
    const std::string path = tempFile("contest_y_random_only");
    {
        std::ofstream out(path);
        out << "axis,type,index\n";
        writeRandom(out, 'y', 100);
    }

    ContestPositions pos;
    std::string error;
    CHECK(parsePositionsFile(path, 1000, 1000, 1000, 100, 10, pos, error), error);
    CHECK(pos.y_random.size() == 100, "Y random-only count");
    CHECK(pos.x_random.empty() && pos.z_random.empty(), "other random groups empty");
    CHECK(pos.x_continuous.empty() && pos.y_continuous.empty() && pos.z_continuous.empty(),
          "all continuous groups empty");
    std::filesystem::remove(path);
}

void testEmptyCsvRejected() {
    const std::string path = tempFile("contest_empty");
    {
        std::ofstream out(path);
        out << "axis,type,index\n";
    }

    ContestPositions pos;
    std::string error;
    CHECK(!parsePositionsFile(path, 1000, 1000, 1000, 100, 10, pos, error),
          "empty CSV must fail");
    CHECK(error.find("no slice requests") != std::string::npos,
          "empty CSV error is explicit");
    std::filesystem::remove(path);
}

void testPresentGroupStillStrict() {
    const std::string path = tempFile("contest_partial_group");
    {
        std::ofstream out(path);
        out << "axis,type,index\n";
        writeRandom(out, 'z', 99);
    }

    ContestPositions pos;
    std::string error;
    CHECK(!parsePositionsFile(path, 1000, 1000, 1000, 100, 10, pos, error),
          "present random group with 99 entries must fail");
    CHECK(error.find("99") != std::string::npos && error.find("100") != std::string::npos,
          "strict count error contains actual and expected counts");
    std::filesystem::remove(path);
}

void testIndependentSingleAxisTiming() {
    ContestPositions pos;
    pos.x_random = {1};

    const std::string outDir = tempDir("contest_sparse_independent");
    ContestUnifiedProfile profile;
    const auto reader = [](
        SliceAxis,
        const std::vector<uint64_t>& indices,
        std::vector<std::vector<float>>& outputs
    ) {
        if (indices.size() != outputs.size()) return false;
        for (auto& output : outputs) {
            std::fill(output.begin(), output.end(), 3.0f);
        }
        return true;
    };

    CHECK(executeContestGroups(pos, outDir, 4, 5, 6, reader, &profile),
          "single-axis independent execution");
    CHECK(profile.output_file_count == 1, "single-axis output count");
    CHECK(profile.x_random.slice_count == 1, "single-axis X random timing present");
    CHECK(profile.y_random.slice_count == 0 && profile.z_random.slice_count == 0,
          "unrequested axes have no timing");
    CHECK(profile.t_x_ms > 0.0, "X axis time is positive");
    CHECK(profile.t_y_ms == 0.0 && profile.t_z_ms == 0.0,
          "unrequested axis times are zero");
    CHECK(std::abs(profile.t_composite_ms - profile.t_x_ms) < 1e-9,
          "composite equals the only requested axis");
    CHECK(std::filesystem::exists(outDir + "/contest_x_random_000.dat"),
          "requested output file exists");
    CHECK(!std::filesystem::exists(outDir + "/contest_y_random_000.dat"),
          "unrequested output file is absent");

    std::filesystem::remove_all(outDir);
}

void testMergedSingleAxisTiming() {
    ContestPositions pos;
    pos.z_continuous = {0, 1};

    const std::string outDir = tempDir("contest_sparse_merged");
    ContestUnifiedProfile profile;
    const auto mergedReader = [](
        const std::vector<GroupReadEntry>& groups,
        std::vector<std::vector<std::vector<float>>>& outputs
    ) {
        if (groups.size() != 1 || groups[0].axis != SliceAxis::Z) return false;
        auto& groupOutputs = outputs[groups[0].original_group_id];
        for (auto& output : groupOutputs) {
            std::fill(output.begin(), output.end(), 7.0f);
        }
        return true;
    };

    CHECK(executeContestGroupsMerged(pos, outDir, 4, 5, 6, mergedReader, &profile),
          "single-axis merged execution");
    CHECK(profile.output_file_count == 2, "merged single-axis output count");
    CHECK(profile.z_continuous.slice_count == 2, "merged Z continuous timing present");
    CHECK(profile.t_x_ms == 0.0 && profile.t_y_ms == 0.0,
          "merged unrequested axis times are zero");
    CHECK(profile.t_z_ms > 0.0, "merged Z axis time is positive");
    CHECK(std::abs(profile.t_composite_ms - profile.t_z_ms) < 1e-9,
          "merged composite equals the only requested axis");

    std::filesystem::remove_all(outDir);
}

} // namespace

int main() {
    testXOnlyCsv();
    testYRandomOnlyCsv();
    testEmptyCsvRejected();
    testPresentGroupStillStrict();
    testIndependentSingleAxisTiming();
    testMergedSingleAxisTiming();

    if (failures == 0) {
        std::cout << "All sparse contest group tests passed\n";
        return 0;
    }

    std::cerr << failures << " sparse contest group test(s) failed\n";
    return 1;
}
