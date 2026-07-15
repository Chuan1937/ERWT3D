#include "erwt3d/rzfp_auto_plan.hpp"

#include <cstdint>
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
    for (uint64_t z = 0; z < nz; ++z) {
        for (uint64_t y = 0; y < ny; ++y) {
            for (uint64_t x = 0; x < nx; ++x) {
                data[(z * ny + y) * nx + x] = static_cast<float>(x * 100 + y * 10 + z);
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
    const std::string path = "/tmp/test_auto_plan.raw";
    const uint64_t nx = 40;
    const uint64_t ny = 48;
    const uint64_t nz = 32;

    check(writeRawFile(path, nx, ny, nz), "write raw file");

    int fd = open(path.c_str(), O_RDONLY);
    check(fd >= 0, "open raw file");

    erwt3d::RzfpAutoPlanConfig cfg;
    cfg.time_limit_seconds = 60;
    cfg.soft_time_limit_seconds = 30;
    cfg.evaluate_x_sidecar = true;
    cfg.main_codec_config.error.contest_bound = 1e-3;
    cfg.main_codec_config.error.internal_bound = 7.5e-4;
    cfg.main_codec_config.error.policy = erwt3d::RelativeErrorPolicy::Strict;
    cfg.sidecar_codec_config.error.contest_bound = 1e-3;
    cfg.sidecar_codec_config.error.internal_bound = 7.5e-4;
    cfg.sidecar_codec_config.error.policy = erwt3d::RelativeErrorPolicy::Strict;

    erwt3d::RzfpAutoPlanResult result;
    check(erwt3d::runRzfpAutoPlan(fd, nx, ny, nz, cfg, result), "runRzfpAutoPlan");
    close(fd);

    check(result.main_ratio_estimate > 0.0 && result.main_ratio_estimate <= 1.0,
          "main ratio in valid range");
    check(result.main_ratio_lower <= result.main_ratio_estimate,
          "main lower <= estimate");
    check(result.main_ratio_upper >= result.main_ratio_estimate,
          "main upper >= estimate");
    check(result.x_sidecar_ratio_estimate > 0.0 && result.x_sidecar_ratio_estimate <= 1.0,
          "sidecar ratio in valid range");
    check(result.sampling_rounds >= 1, "at least one sampling round");
    check(result.elapsed_seconds > 0.0, "elapsed time recorded");
    check(result.elapsed_seconds < cfg.time_limit_seconds, "elapsed under hard limit");

    const std::string json = result.toJson();
    check(!json.empty(), "JSON output not empty");
    check(json.find("main_ratio") != std::string::npos, "JSON contains main_ratio");

    unlink(path.c_str());

    if (g_failures == 0) {
        std::cout << "PASS" << std::endl;
        return 0;
    }
    std::cerr << g_failures << " failures" << std::endl;
    return 1;
}
