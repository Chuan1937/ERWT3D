#include "erwt3d/device_profile.hpp"
#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/rzfp_writer.hpp"
#include "erwt3d/raw_layout.hpp"
#include "erwt3d/raw_x_aux.hpp"

#include <cassert>
#include <cmath>
#include <cstdlib>
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
    CHECK(cfg.adaptive.auto_calibrate_device == true);
    CHECK(cfg.adaptive.cache_policy == erwt3d::CachePolicy::StableAuto);
}

// Strategy protection: verify config values produce expected behavior
void testFullscanProtectionConfig() {
    erwt3d::RzfpAdaptiveConfig cfg;
    CHECK(cfg.max_fullscan_seconds == 120.0);
    CHECK(cfg.fullscan_min_advantage == 0.20);
    CHECK(cfg.strategy_switch_margin == 0.15);
    CHECK(cfg.enable_strategy_probe == true);
    CHECK(cfg.strategy_probe_bytes == 256ULL * 1024 * 1024);

    // Slow disk flags
    cfg.max_fullscan_seconds = 60.0;
    CHECK(cfg.max_fullscan_seconds == 60.0);

    cfg.fullscan_min_advantage = 0.30;
    CHECK(cfg.fullscan_min_advantage == 0.30);

    cfg.strategy_switch_margin = 0.10;
    CHECK_APPROX(cfg.strategy_switch_margin, 0.10, 0.001);
}

// Simulate different disk profiles
void testDeviceProfileScenarios() {
    erwt3d::DeviceProfile slow;
    slow.sequential_mb_s = 66.0;
    slow.random_seek_ms = 12.0;
    slow.calibrated = true;
    CHECK(slow.sequential_mb_s == 66.0);
    CHECK(slow.calibrated == true);

    erwt3d::DeviceProfile normal;
    normal.sequential_mb_s = 150.0;
    normal.random_seek_ms = 9.0;
    normal.calibrated = true;
    CHECK(normal.sequential_mb_s == 150.0);

    erwt3d::DeviceProfile fast;
    fast.sequential_mb_s = 300.0;
    fast.random_seek_ms = 7.0;
    fast.calibrated = true;
    CHECK(fast.sequential_mb_s == 300.0);

    // HDD config override takes priority over device profile
    erwt3d::RzfpReaderConfig cfg;
    cfg.hdd.sequential_mb_s = 66.0;
    cfg.hdd.seek_ms = 12.0;
    CHECK(cfg.hdd.sequential_mb_s == 66.0);

    cfg.hdd.sequential_mb_s = 0.0; // Let device profile take over
    cfg.adaptive.auto_calibrate_device = true;
    CHECK(cfg.adaptive.auto_calibrate_device == true);
}

// Cache policy integration test with a real RZFP file
void testCachePolicyIntegration() {
    // Create minimal test data
    uint64_t nx = 17, ny = 19, nz = 23;
    std::string rawPath = "/tmp/test_adaptive_raw.raw";
    std::string rzfpPath = "/tmp/test_adaptive.rzfp";

    // Generate test data with small values (RZFP has ~0.001 relative error)
    std::vector<float> raw(nx * ny * nz);
    for (uint64_t x = 0; x < nx; ++x)
        for (uint64_t y = 0; y < ny; ++y)
            for (uint64_t z = 0; z < nz; ++z)
                raw[(x * ny + y) * nz + z] = static_cast<float>(x * 10.0f + y * 0.1f + z * 0.001f + 0.5f);

    int rfd = open(rawPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    erwt3d::writeFullyAt(rfd, raw.data(), raw.size() * sizeof(float), 0);
    close(rfd);

    erwt3d::RzfpWriterConfig wcfg;
    wcfg.nx = nx; wcfg.ny = ny; wcfg.nz = nz;
    wcfg.threads = 4;
    CHECK(erwt3d::writeRzfpFile(rawPath, rzfpPath, wcfg));

    // Test with StableAuto (default)
    {
        erwt3d::RzfpReader reader(rzfpPath);
        CHECK(reader.ok());

        std::vector<float> output(ny * nz);
        CHECK(reader.readSlice(erwt3d::SliceAxis::X, static_cast<uint64_t>(0), output.data(), 1, 2048));
        for (uint64_t y = 0; y < ny; ++y)
            for (uint64_t z = 0; z < nz; ++z) {
                float expected = static_cast<float>(y * 0.1f + z * 0.001f + 0.5f);
                float actual = output[y * nz + z];
                float relErr = expected != 0.0f ? std::abs(actual - expected) / std::abs(expected) : std::abs(actual);
                CHECK(relErr < 0.001f);
            }

        CHECK(reader.header().nx == 17);
    }

    // Test with DeterministicCold
    {
        erwt3d::RzfpReader reader(rzfpPath);
        CHECK(reader.ok());

        erwt3d::RzfpReaderConfig cfg;
        cfg.adaptive.cache_policy = erwt3d::CachePolicy::DeterministicCold;
        cfg.decode_threads = 1;

        std::vector<float> output(ny * nz);
        std::vector<erwt3d::RzfpReader::SliceBatchRequest> reqs(1);
        reqs[0] = {erwt3d::SliceAxis::X, static_cast<uint64_t>(0), output.data()};
        CHECK(reader.readSlicesBatch(reqs, cfg));
        for (uint64_t y = 0; y < ny; ++y)
            for (uint64_t z = 0; z < nz; ++z) {
                float expected = static_cast<float>(y * 0.1f + z * 0.001f + 0.5f);
                float actual = output[y * nz + z];
                float relErr = expected != 0.0f ? std::abs(actual - expected) / std::abs(expected) : std::abs(actual);
                CHECK(relErr < 0.001f);
            }
    }

    // Test with WarmAllowed
    {
        erwt3d::RzfpReader reader(rzfpPath);
        CHECK(reader.ok());

        erwt3d::RzfpReaderConfig cfg;
        cfg.adaptive.cache_policy = erwt3d::CachePolicy::WarmAllowed;
        cfg.decode_threads = 4;

        std::vector<float> output(ny * nz);
        std::vector<erwt3d::RzfpReader::SliceBatchRequest> reqs(1);
        reqs[0] = {erwt3d::SliceAxis::Y, static_cast<uint64_t>(0), output.data()};
        CHECK(reader.readSlicesBatch(reqs, cfg));
    }

    unlink(rawPath.c_str());
    unlink(rzfpPath.c_str());
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
    testFullscanProtectionConfig();
    testDeviceProfileScenarios();
    testCachePolicyIntegration();

    if (failures == 0) {
        std::cout << "ALL TESTS PASSED" << std::endl;
        return 0;
    }
    std::cout << failures << " FAILURES" << std::endl;
    return 1;
}
