#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/rzfp_writer.hpp"

#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* msg) {
    if (!condition) {
        std::cerr << "FAIL: " << msg << std::endl;
        ++g_failures;
    }
}

bool writeRawFile(const std::string& path, uint64_t nx, uint64_t ny, uint64_t nz) {
    std::vector<float> data(nx * ny * nz);
    for (uint64_t x = 0; x < nx; ++x) {
        for (uint64_t y = 0; y < ny; ++y) {
            for (uint64_t z = 0; z < nz; ++z) {
                float v = static_cast<float>(x * 100 + y * 10 + z) / 1000.0f;
                // Add a bit of high-frequency variation so compression isn't trivial.
                v += static_cast<float>((x + y + z) % 7) * 0.01f;
                data[(x * ny + y) * nz + z] = v;
            }
        }
    }
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    size_t done = 0;
    const size_t total = data.size() * sizeof(float);
    while (done < total) {
        ssize_t n = write(fd, reinterpret_cast<const uint8_t*>(data.data()) + done, total - done);
        if (n <= 0) {
            close(fd);
            return false;
        }
        done += static_cast<size_t>(n);
    }
    close(fd);
    return true;
}

bool readSliceStrategy(const std::string& path,
                       erwt3d::SliceAxis axis, uint64_t index,
                       erwt3d::RzfpReadStrategy strategy,
                       std::vector<float>& out,
                       erwt3d::RzfpReadProfile& profile) {
    erwt3d::RzfpReader reader(path);
    if (!reader.ok()) return false;

    const auto& header = reader.header();
    uint64_t slice_size = 0;
    switch (axis) {
        case erwt3d::SliceAxis::X: slice_size = header.ny * header.nz; break;
        case erwt3d::SliceAxis::Y: slice_size = header.nx * header.nz; break;
        case erwt3d::SliceAxis::Z: slice_size = header.nx * header.ny; break;
    }
    out.resize(slice_size);

    erwt3d::RzfpReaderConfig config;
    config.strategy = strategy;
    config.decode_threads = 4;
    config.profile = &profile;

    std::vector<erwt3d::RzfpReader::SliceBatchRequest> reqs = {{axis, index, out.data()}};
    return reader.readSlicesBatch(reqs, config);
}

bool slicesEqual(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::isnan(a[i]) || std::isnan(b[i])) return false;
        if (a[i] != b[i]) return false;
    }
    return true;
}

const char* strategyName(erwt3d::RzfpReadStrategy s) {
    switch (s) {
        case erwt3d::RzfpReadStrategy::SelectiveLeaf: return "selective";
        case erwt3d::RzfpReadStrategy::WholeSuperblock: return "whole";
        case erwt3d::RzfpReadStrategy::FullPayloadScan: return "fullscan";
        default: return "auto";
    }
}

void testStrategyConsistency(const std::string& path,
                             erwt3d::SliceAxis axis, uint64_t index) {
    std::vector<float> reference;
    erwt3d::RzfpReadProfile ref_profile;
    check(readSliceStrategy(path, axis, index,
                            erwt3d::RzfpReadStrategy::SelectiveLeaf,
                            reference, ref_profile),
          "selective read succeeded");

    std::vector<float> whole;
    erwt3d::RzfpReadProfile whole_profile;
    check(readSliceStrategy(path, axis, index,
                            erwt3d::RzfpReadStrategy::WholeSuperblock,
                            whole, whole_profile),
          "whole read succeeded");
    check(slicesEqual(reference, whole), "whole matches selective");

    std::vector<float> fullscan;
    erwt3d::RzfpReadProfile fullscan_profile;
    check(readSliceStrategy(path, axis, index,
                            erwt3d::RzfpReadStrategy::FullPayloadScan,
                            fullscan, fullscan_profile),
          "fullscan read succeeded");
    check(slicesEqual(reference, fullscan), "fullscan matches selective");

    std::vector<float> auto_slice;
    erwt3d::RzfpReadProfile auto_profile;
    check(readSliceStrategy(path, axis, index,
                            erwt3d::RzfpReadStrategy::Auto,
                            auto_slice, auto_profile),
          "auto read succeeded");
    check(slicesEqual(reference, auto_slice), "auto matches selective");

    std::cout << "  axis=" << static_cast<int>(axis) << " index=" << index
              << " auto_strategy=" << strategyName(auto_profile.selected_strategy)
              << " selective_io=" << ref_profile.io_time_ms << "ms"
              << " whole_io=" << whole_profile.io_time_ms << "ms"
              << " fullscan_io=" << fullscan_profile.io_time_ms << "ms\n";
}

} // namespace

int main() {
    std::system("mkdir -p /mnt/d/opencode_tests");
    const std::string raw_path = "/mnt/d/opencode_tests/test_reader_strategies.raw";
    const std::string rzfp_path = "/mnt/d/opencode_tests/test_reader_strategies.rzfp";

    const uint64_t nx = 48;
    const uint64_t ny = 56;
    const uint64_t nz = 40;

    check(writeRawFile(raw_path, nx, ny, nz), "write raw file");

    erwt3d::RzfpWriterConfig wcfg;
    wcfg.nx = nx;
    wcfg.ny = ny;
    wcfg.nz = nz;
    wcfg.super_size = 32;
    wcfg.leaf_size = 4;
    wcfg.threads = 4;
    wcfg.memory_limit_mb = 1024;
    wcfg.codec.error.contest_bound = 1e-3;
    wcfg.codec.error.internal_bound = 7.5e-4;
    wcfg.codec.error.policy = erwt3d::RelativeErrorPolicy::Strict;

    erwt3d::RzfpWriterStats stats;
    check(erwt3d::writeRzfpFile(raw_path, rzfp_path, wcfg, &stats), "write RZFP file");
    check(stats.violation_count == 0, "no relative-error violations");

    testStrategyConsistency(rzfp_path, erwt3d::SliceAxis::X, nx / 2);
    testStrategyConsistency(rzfp_path, erwt3d::SliceAxis::Y, ny / 2);
    testStrategyConsistency(rzfp_path, erwt3d::SliceAxis::Z, nz / 2);

    unlink(raw_path.c_str());
    unlink(rzfp_path.c_str());

    if (g_failures == 0) {
        std::cout << "PASS" << std::endl;
        return 0;
    }
    std::cerr << g_failures << " failures" << std::endl;
    return 1;
}
