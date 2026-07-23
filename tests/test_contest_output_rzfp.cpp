#include "erwt3d/rzfp_writer.hpp"
#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/contest_positions.hpp"
#include "erwt3d/contest_groups.hpp"
#include "erwt3d/relative_error.hpp"
#include "erwt3d/raw_layout.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
    data[rawOffsetZFastest(0, 0, 0, ny, nz)] = 0.0f;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()),
              data.size() * sizeof(float));
}

static std::string makePath(const std::string& dir, const std::string& name,
                             size_t index)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s/contest_%s_%03zu.dat",
             dir.c_str(), name.c_str(), index);
    return std::string(buf);
}

static void testRzfpContestOutput() {
    const char* rawPath = "/tmp/test_cout_rzfp.raw";
    const char* rzfpPath = "/tmp/test_cout_rzfp.rzfp";
    const char* outDir = "/tmp/test_cout_rzfp_out";

    const uint64_t nx = 80;
    const uint64_t ny = 60;
    const uint64_t nz = 40;
    writeRawFile(rawPath, nx, ny, nz);

    RzfpWriterConfig cfg;
    cfg.nx = nx; cfg.ny = ny; cfg.nz = nz;
    cfg.threads = 2;
    cfg.memory_limit_mb = 256;
    cfg.codec.error.policy = RelativeErrorPolicy::Strict;
    cfg.codec.error.contest_bound = 1e-3;
    cfg.codec.error.internal_bound = 7.5e-4;
    CHECK(writeRzfpFile(rawPath, rzfpPath, cfg), "write RZFP");

    ContestPositions positions;
    std::string err;
    CHECK(generateRandomPositions(nx, ny, nz, 10, 5, 42, positions, err), err);

    std::error_code ec;
    if (!std::filesystem::is_directory(outDir, ec)) {
        std::filesystem::create_directory(outDir, ec);
    }

    std::vector<float> rawData(nx * ny * nz);
    {
        std::ifstream in(rawPath, std::ios::binary);
        in.read(reinterpret_cast<char*>(rawData.data()),
                rawData.size() * sizeof(float));
    }

    auto rzfpReader = std::make_shared<RzfpReader>(rzfpPath);
    CHECK(rzfpReader->ok(), "open RZFP reader");

    RzfpReaderConfig rcfg;
    rcfg.decode_threads = 2;

    MultiGroupReadFunction mergedFn =
        [&](const std::vector<GroupReadEntry>& groups,
            std::vector<std::vector<std::vector<float>>>& allOutputs) -> bool {
        if (groups.empty()) return true;

        std::vector<RzfpReader::ContestRoundGroup> cgroups(groups.size());
        for (size_t g = 0; g < groups.size(); ++g) {
            cgroups[g].axis = groups[g].axis;
            cgroups[g].name = groups[g].name;
            cgroups[g].indices = groups[g].indices;
            cgroups[g].outputs.clear();
            for (auto& o : allOutputs[groups[g].original_group_id])
                cgroups[g].outputs.push_back(o.data());
        }

        std::vector<RzfpReader::RzfpRoundReadResult> results;
        return rzfpReader->readContestRound(cgroups, rcfg, &results);
    };

    ContestUnifiedProfile profile;
    CHECK(executeContestGroupsMerged(positions, outDir, nx, ny, nz,
                                       mergedFn, &profile),
          "execute merged contest");

    CHECK(profile.output_file_count == 45,
          "correct number of output files (45 = 3*10+3*5)");

    CHECK(profile.output_total_bytes > 0, "output bytes > 0");

    std::string names[6] = {
        "x_random", "y_random", "z_random",
        "x_continuous", "y_continuous", "z_continuous"
    };
    SliceAxis axes[6] = {
        SliceAxis::X, SliceAxis::Y, SliceAxis::Z,
        SliceAxis::X, SliceAxis::Y, SliceAxis::Z
    };
    const std::vector<uint64_t>* idxs[6] = {
        &positions.x_random, &positions.y_random, &positions.z_random,
        &positions.x_continuous, &positions.y_continuous, &positions.z_continuous
    };

    uint64_t violations = 0;
    size_t datCount = 0;
    for (int g = 0; g < 6; ++g) {
        for (size_t i = 0; i < idxs[g]->size(); ++i) {
            std::string path = makePath(outDir, names[g], i);
            std::error_code ec2;
            CHECK(std::filesystem::is_regular_file(path, ec2),
                  (path + " exists").c_str());
            ++datCount;

            uint64_t expectedSize = 0;
            if (axes[g] == SliceAxis::X) expectedSize = ny * nz * sizeof(float);
            else if (axes[g] == SliceAxis::Y) expectedSize = nx * nz * sizeof(float);
            else expectedSize = nx * ny * sizeof(float);
            uint64_t actualSize = std::filesystem::file_size(path, ec2);
            CHECK(actualSize == expectedSize,
                  (path + " correct size").c_str());

            std::vector<float> output(expectedSize / sizeof(float));
            {
                std::ifstream in(path, std::ios::binary);
                in.read(reinterpret_cast<char*>(output.data()), expectedSize);
            }

            uint64_t idx = (*idxs[g])[i];
            for (size_t k = 0; k < output.size(); ++k) {
                double orig;
                if (axes[g] == SliceAxis::X) {
                    uint64_t y = k / nz;
                    uint64_t z = k % nz;
                    orig = rawData[rawOffsetZFastest(idx, y, z, ny, nz)];
                } else if (axes[g] == SliceAxis::Y) {
                    uint64_t x = k / nz;
                    uint64_t z = k % nz;
                    orig = rawData[rawOffsetZFastest(x, idx, z, ny, nz)];
                } else {
                    uint64_t x = k / ny;
                    uint64_t y = k % ny;
                    orig = rawData[rawOffsetZFastest(x, y, idx, ny, nz)];
                }

                double val = static_cast<double>(output[k]);
                if (orig == 0.0) {
                    if (val != 0.0) ++violations;
                } else {
                    double relErr = std::abs(val - orig) /
                                    std::max(1e-12, std::abs(orig));
                    if (relErr >= 1e-3) ++violations;
                }
            }
        }
    }

    CHECK(violations == 0, "no violations in RZFP DAT output");
    CHECK(datCount == 45, "total DAT file count = 45");

    for (int g = 0; g < 6; ++g) {
        for (size_t i = 0; i < idxs[g]->size(); ++i) {
            std::string path = makePath(outDir, names[g], i);
            unlink(path.c_str());
        }
    }
    std::filesystem::remove_all(outDir, ec);

    unlink(rawPath);
    unlink(rzfpPath);
}

int main() {
    testRzfpContestOutput();

    std::cout << "Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed > 0 ? 1 : 0;
}
