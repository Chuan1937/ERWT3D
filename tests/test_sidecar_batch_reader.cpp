#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/rzfp_writer.hpp"
#include "erwt3d/rzfp_xplane_writer.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
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
                data[(x * ny + y) * nz + z] = std::sin(x * 0.1f) * std::cos(y * 0.05f) * std::sin(z * 0.07f);
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

} // namespace

int main() {
    std::system("mkdir -p /mnt/d/opencode_tests");
    const std::string raw_path = "/mnt/d/opencode_tests/test_batch_raw.raw";
    const std::string rzfp_path = "/mnt/d/opencode_tests/test_batch_raw.rzfp";
    const std::string xp_path = rzfp_path + ".xp";
    const uint64_t nx = 48;
    const uint64_t ny = 40;
    const uint64_t nz = 32;

    check(writeRawFile(raw_path, nx, ny, nz), "write raw file");

    erwt3d::RzfpCodecConfig codec_cfg;
    codec_cfg.error.contest_bound = 1e-3;
    codec_cfg.error.internal_bound = 7.5e-4;
    codec_cfg.error.policy = erwt3d::RelativeErrorPolicy::Strict;

    erwt3d::RzfpWriterConfig writer_cfg;
    writer_cfg.nx = nx;
    writer_cfg.ny = ny;
    writer_cfg.nz = nz;
    writer_cfg.threads = 4;
    writer_cfg.codec = codec_cfg;
    check(erwt3d::writeRzfpFile(raw_path, rzfp_path, writer_cfg, nullptr),
          "write RZFP main file");

    erwt3d::RzfpXPlaneCodecConfig xp_cfg;
    xp_cfg.error = codec_cfg.error;
    xp_cfg.fast_accuracy_only = true;
    check(erwt3d::writeXPlaneSidecarFile(raw_path, xp_path, xp_cfg, nx, ny, nz, 4, nullptr),
          "write sidecar file");

    erwt3d::RzfpReader reader(rzfp_path);
    check(reader.ok(), "reader open");

    std::vector<uint64_t> x_indices = {5, 12, 23, 1, 47, 30, 8};
    std::vector<std::vector<float>> outputs(x_indices.size(), std::vector<float>(ny * nz));
    std::vector<erwt3d::RzfpReader::SliceBatchRequest> reqs;
    for (size_t i = 0; i < x_indices.size(); ++i) {
        reqs.push_back({erwt3d::SliceAxis::X, x_indices[i], outputs[i].data()});
    }

    erwt3d::RzfpReaderConfig cfg;
    cfg.decode_threads = 2;
    cfg.hdd.read_window_bytes = 64 * 1024;
    cfg.hdd.max_gap_bytes = 4096;
    erwt3d::RzfpReadProfile profile;
    cfg.profile = &profile;

    check(reader.readSlicesBatch(reqs, cfg), "batch read X slices");

    for (size_t i = 0; i < x_indices.size(); ++i) {
        const uint64_t x = x_indices[i];
        for (uint64_t y = 0; y < ny; ++y) {
            for (uint64_t z = 0; z < nz; ++z) {
                const float expected = std::sin(x * 0.1f) * std::cos(y * 0.05f) * std::sin(z * 0.07f);
                const float actual = outputs[i][y * nz + z];
                const double denom = std::max(1.0, std::abs(static_cast<double>(expected)));
                const double rel = std::abs(static_cast<double>(actual - expected)) / denom;
                check(rel < 1e-3, "reconstructed value within tolerance");
            }
        }
    }

    check(profile.pread_calls <= static_cast<uint64_t>(x_indices.size()),
          "batch read merged preads");

    close(open(raw_path.c_str(), O_RDONLY));
    unlink(raw_path.c_str());
    unlink(rzfp_path.c_str());
    unlink(xp_path.c_str());

    if (g_failures == 0) {
        std::cout << "PASS" << std::endl;
        return 0;
    }
    std::cerr << g_failures << " failures" << std::endl;
    return 1;
}
