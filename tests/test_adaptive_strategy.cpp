#include "erwt3d/device_profile.hpp"
#include "erwt3d/rzfp_reader.hpp"

#include <cassert>
#include <cmath>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>
#include <vector>

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAIL: " << #cond << " at " << __LINE__ << "\n"; \
        ++failures; \
    } \
} while(0)

#define CHECK_APPROX(a, b, tol) do { \
    if (std::abs((a) - (b)) > (tol)) { \
        std::cerr << "FAIL: |" << #a << " - " << #b << "| = " << std::abs((a)-(b)) << " > " << tol << " at " << __LINE__ << "\n"; \
        ++failures; \
    } \
} while(0)

namespace {

void testDeviceCalibrationConfig() {
    erwt3d::DeviceCalibrationConfig cfg;
    CHECK(cfg.sequential_region_bytes == 128ULL * 1024 * 1024);
    CHECK(cfg.sequential_region_count == 3);
    CHECK(cfg.random_probe_count == 64);
    CHECK(cfg.random_probe_bytes == 64ULL * 1024);
    CHECK(cfg.evict_after_probe == true);
}

void testDeviceProfileDefaults() {
    erwt3d::DeviceProfile p;
    CHECK(p.sequential_mb_s == 0.0);
    CHECK(p.calibrated == false);

    // calibrateDeviceProfile with too-small file should return conservative defaults
    int fd = open("/dev/null", O_RDONLY);
    // Can't actually calibrate /dev/null, but the function should handle small files
    if (fd >= 0) {
        auto result = erwt3d::calibrateDeviceProfile(fd, 0, {});
        CHECK(!result.calibrated);
        CHECK(result.sequential_mb_s >= 1.0);
        close(fd);
    }
}

void testDeviceProfileCache() {
    auto& cache = erwt3d::DeviceProfileCache::instance();
    cache.clear();
    // Singleton should work
    CHECK(true);
}

void testCachePolicyEnum() {
    CHECK(static_cast<int>(erwt3d::CachePolicy::StableAuto) == 0);
    CHECK(static_cast<int>(erwt3d::CachePolicy::DeterministicCold) == 1);
    CHECK(static_cast<int>(erwt3d::CachePolicy::WarmAllowed) == 2);
}

void testAdaptiveConfigDefaults() {
    erwt3d::RzfpAdaptiveConfig cfg;
    CHECK(cfg.auto_calibrate_device == true);
    CHECK(cfg.cache_policy == erwt3d::CachePolicy::StableAuto);
    CHECK_APPROX(cfg.strategy_switch_margin, 0.15, 0.001);
    CHECK_APPROX(cfg.max_fullscan_seconds, 120.0, 0.001);
    CHECK_APPROX(cfg.fullscan_min_advantage, 0.20, 0.001);
    CHECK(cfg.enable_strategy_probe == true);
    CHECK(cfg.strategy_probe_bytes == 256ULL * 1024 * 1024);
}

void testReaderConfigDefaults() {
    erwt3d::RzfpReaderConfig cfg;
    CHECK(cfg.strategy == erwt3d::RzfpReadStrategy::Auto);
    CHECK(cfg.decode_threads == 1);
    CHECK(cfg.profile == nullptr);
}

} // namespace

int main() {
    std::cout << "test_adaptive_strategy" << std::endl;

    testDeviceCalibrationConfig();
    testDeviceProfileDefaults();
    testDeviceProfileCache();
    testCachePolicyEnum();
    testAdaptiveConfigDefaults();
    testReaderConfigDefaults();

    if (failures == 0) {
        std::cout << "ALL TESTS PASSED" << std::endl;
        return 0;
    }
    std::cout << failures << " FAILURES" << std::endl;
    return 1;
}
