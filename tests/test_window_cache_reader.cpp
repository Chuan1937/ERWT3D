#include "erwt3d/raw_x_aux.hpp"
#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/rzfp_writer.hpp"
#include "erwt3d/window_cache.hpp"

#include <cmath>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << "FAIL: " << #condition \
                  << " at line " << __LINE__ << "\n"; \
        ++failures; \
    } \
} while (false)

void checkYPlane(
    const std::vector<float>& output,
    uint64_t nx,
    uint64_t nz,
    uint64_t y
) {
    for (uint64_t x = 0; x < nx; ++x) {
        for (uint64_t z = 0; z < nz; ++z) {
            const float expected = static_cast<float>(
                1.0 + x * 0.01 + y * 0.001 + z * 0.0001
            );
            const float actual = output[x * nz + z];
            const float relative = std::abs(actual - expected) /
                                   std::max(std::abs(expected), 1e-6f);
            CHECK(relative < 0.001f);
        }
    }
}

void testWindowCacheHitAndClear() {
    const uint64_t nx = 65;
    const uint64_t ny = 66;
    const uint64_t nz = 67;
    const uint64_t y = 17;

    const std::string rawPath = "/tmp/erwt3d_window_cache.raw";
    const std::string rzfpPath = "/tmp/erwt3d_window_cache.rzfp";

    std::vector<float> raw(nx * ny * nz);
    for (uint64_t x = 0; x < nx; ++x) {
        for (uint64_t yy = 0; yy < ny; ++yy) {
            for (uint64_t z = 0; z < nz; ++z) {
                raw[(x * ny + yy) * nz + z] = static_cast<float>(
                    1.0 + x * 0.01 + yy * 0.001 + z * 0.0001
                );
            }
        }
    }

    const int rawFd = open(
        rawPath.c_str(),
        O_WRONLY | O_CREAT | O_TRUNC,
        0644
    );
    CHECK(rawFd >= 0);
    if (rawFd < 0) return;
    CHECK(erwt3d::writeFullyAt(
        rawFd,
        raw.data(),
        raw.size() * sizeof(float),
        0
    ));
    close(rawFd);

    erwt3d::RzfpWriterConfig writerConfig;
    writerConfig.nx = nx;
    writerConfig.ny = ny;
    writerConfig.nz = nz;
    writerConfig.threads = 4;
    CHECK(erwt3d::writeRzfpFile(rawPath, rzfpPath, writerConfig));

    erwt3d::RzfpReader reader(rzfpPath);
    CHECK(reader.ok());
    if (!reader.ok()) {
        unlink(rawPath.c_str());
        unlink(rzfpPath.c_str());
        return;
    }

    auto cache = std::make_shared<erwt3d::BoundedWindowCache>(
        64ULL * 1024 * 1024
    );

    erwt3d::RzfpReaderConfig config;
    config.strategy = erwt3d::RzfpReadStrategy::WholeSuperblock;
    config.decode_threads = 2;
    config.hdd.read_window_bytes = 16ULL * 1024 * 1024;
    config.hdd.max_gap_bytes = 1ULL * 1024 * 1024;
    config.window_cache = cache;
    config.window_cache_file_identity = reader.fileIdentity();
    config.use_window_cache = true;
    config.adaptive.cache_policy = erwt3d::CachePolicy::StableAuto;

    std::vector<float> first(nx * nz);
    erwt3d::RzfpReadProfile firstProfile;
    config.profile = &firstProfile;
    std::vector<erwt3d::RzfpReader::SliceBatchRequest> firstRequests = {
        {erwt3d::SliceAxis::Y, y, first.data()}
    };
    CHECK(reader.readSlicesBatch(firstRequests, config));
    checkYPlane(first, nx, nz, y);
    CHECK(firstProfile.window_cache_misses > 0);
    CHECK(firstProfile.actual_read_bytes > 0);
    CHECK(cache->residentBytes() > 0);

    std::vector<float> second(nx * nz);
    erwt3d::RzfpReadProfile secondProfile;
    config.profile = &secondProfile;
    std::vector<erwt3d::RzfpReader::SliceBatchRequest> secondRequests = {
        {erwt3d::SliceAxis::Y, y, second.data()}
    };
    CHECK(reader.readSlicesBatch(secondRequests, config));
    checkYPlane(second, nx, nz, y);
    CHECK(secondProfile.window_cache_hits > 0);
    CHECK(secondProfile.window_cache_saved_read_bytes > 0);
    CHECK(secondProfile.actual_read_bytes == 0);

    cache->clear();
    (void)reader.dropPayloadCache();

    std::vector<float> third(nx * nz);
    erwt3d::RzfpReadProfile thirdProfile;
    config.profile = &thirdProfile;
    std::vector<erwt3d::RzfpReader::SliceBatchRequest> thirdRequests = {
        {erwt3d::SliceAxis::Y, y, third.data()}
    };
    CHECK(reader.readSlicesBatch(thirdRequests, config));
    checkYPlane(third, nx, nz, y);
    CHECK(thirdProfile.window_cache_misses > 0);
    CHECK(thirdProfile.actual_read_bytes > 0);

    unlink(rawPath.c_str());
    unlink(rzfpPath.c_str());
}

} // namespace

int main() {
    std::cout << "test_window_cache_reader\n";
    testWindowCacheHitAndClear();

    if (failures == 0) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    }

    std::cerr << failures << " TESTS FAILED\n";
    return 1;
}
