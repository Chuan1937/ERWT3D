#include "erwt3d/device_profile.hpp"
#include "erwt3d/raw_x_aux.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <random>
#include <sys/stat.h>
#include <vector>

namespace erwt3d {

namespace {

using Clock = std::chrono::steady_clock;

static DeviceKey makeDeviceKey(int fd) {
    DeviceKey key;
    struct _stat64 st{};
    if (fstat(fd, &st) == 0) {
        key.device_id = static_cast<uint64_t>(st.st_dev);
        key.filesystem_id = 0;
    }
    return key;
}

static void dropCachedRange(int fd, uint64_t offset, uint64_t bytes) {
    if (fd < 0 || bytes == 0) return;
    (void)posix_fadvise(
        fd,
        static_cast<int64_t>(offset),
        static_cast<int64_t>(bytes),
        POSIX_FADV_DONTNEED
    );
}

} // namespace

DeviceProfile calibrateDeviceProfile(
    int fd,
    uint64_t file_size,
    const DeviceCalibrationConfig& config
) {
    DeviceProfile profile;

    struct _stat64 st{};
    if (fstat(fd, &st) == 0) {
        profile.device_id = static_cast<uint64_t>(st.st_dev);
    }

    const bool sequentialTooSmall =
        config.sequential_region_bytes == 0 ||
        file_size < config.sequential_region_bytes * 2;
    const bool randomTooSmall =
        config.random_probe_bytes == 0 ||
        file_size < config.random_probe_bytes * 4;

    if (fd < 0 || sequentialTooSmall || randomTooSmall) {
        profile.sequential_mb_s = 80.0;
        profile.minimum_sequential_mb_s = 80.0;
        profile.maximum_sequential_mb_s = 80.0;
        profile.random_seek_ms = 12.0;
        profile.calibrated = false;
        return profile;
    }

    {
        const uint64_t regionBytes = config.sequential_region_bytes;
        const uint32_t count = std::min<uint32_t>(config.sequential_region_count, 3);
        const uint64_t stride = file_size / (static_cast<uint64_t>(count) + 1);

        std::vector<double> speeds;
        speeds.reserve(count);
        std::vector<uint8_t> buffer(regionBytes);

        for (uint32_t i = 0; i < count; ++i) {
            uint64_t offset = stride * (static_cast<uint64_t>(i) + 1);
            if (offset + regionBytes > file_size) {
                offset = file_size - regionBytes;
            }

            if (config.evict_before_probe) {
                dropCachedRange(fd, offset, regionBytes);
            }

            const auto t0 = Clock::now();
            const bool ok = readFullyAt(fd, buffer.data(), regionBytes, offset);
            const auto t1 = Clock::now();

            if (ok) {
                const double elapsed =
                    std::chrono::duration<double>(t1 - t0).count();
                if (elapsed > 0.0) {
                    const double mbPerSecond =
                        (static_cast<double>(regionBytes) /
                         (1024.0 * 1024.0)) /
                        elapsed;
                    speeds.push_back(mbPerSecond);
                }
            }

            if (config.evict_after_probe) {
                dropCachedRange(fd, offset, regionBytes);
            }
        }

        if (!speeds.empty()) {
            std::sort(speeds.begin(), speeds.end());
            profile.minimum_sequential_mb_s = speeds.front();
            profile.maximum_sequential_mb_s = speeds.back();

            const double median = speeds[speeds.size() / 2];
            profile.sequential_mb_s = median;

            if (profile.maximum_sequential_mb_s >
                std::max(1.0, profile.minimum_sequential_mb_s) * 1.75) {
                profile.cache_contamination_suspected = true;
                profile.cached_mb_s = profile.maximum_sequential_mb_s;
                profile.sequential_mb_s = profile.minimum_sequential_mb_s;
            } else if (profile.maximum_sequential_mb_s > median * 1.5) {
                profile.cached_mb_s = profile.maximum_sequential_mb_s;
            }
        } else {
            profile.sequential_mb_s = 80.0;
            profile.minimum_sequential_mb_s = 80.0;
            profile.maximum_sequential_mb_s = 80.0;
        }
    }

    {
        const uint64_t probeBytes = config.random_probe_bytes;
        const uint32_t probeCount = config.random_probe_count;
        const uint64_t region =
            std::min<uint64_t>(file_size - probeBytes, file_size / 2);
        const uint64_t baseOffset = file_size / 4;

        std::mt19937_64 rng(20260511);
        std::uniform_int_distribution<uint64_t> distribution(0, region - 1);

        std::vector<uint8_t> probeBuffer(probeBytes);
        std::vector<double> seekTimes;
        seekTimes.reserve(probeCount);

        for (uint32_t i = 0; i < probeCount; ++i) {
            uint64_t offset = baseOffset + distribution(rng);
            if (offset + probeBytes > file_size) {
                offset = file_size - probeBytes;
            }

            if (config.evict_before_probe) {
                dropCachedRange(fd, offset, probeBytes);
            }

            const auto t0 = Clock::now();
            const bool ok = readFullyAt(
                fd,
                probeBuffer.data(),
                probeBytes,
                offset
            );
            const auto t1 = Clock::now();

            if (ok) {
                const double elapsedMs =
                    std::chrono::duration<double, std::milli>(t1 - t0).count();
                const double transferMs =
                    (static_cast<double>(probeBytes) / (1024.0 * 1024.0)) /
                    std::max(profile.sequential_mb_s, 1.0) * 1000.0;
                seekTimes.push_back(std::max(0.0, elapsedMs - transferMs));
            }

            if (config.evict_after_probe) {
                dropCachedRange(fd, offset, probeBytes);
            }
        }

        if (!seekTimes.empty()) {
            std::sort(seekTimes.begin(), seekTimes.end());
            const size_t trim = seekTimes.size() / 10;
            if (trim * 2 < seekTimes.size()) {
                double sum = 0.0;
                for (size_t i = trim; i < seekTimes.size() - trim; ++i) {
                    sum += seekTimes[i];
                }
                profile.random_seek_ms =
                    sum / static_cast<double>(seekTimes.size() - 2 * trim);
            } else {
                profile.random_seek_ms = seekTimes[seekTimes.size() / 2];
            }
        } else {
            profile.random_seek_ms = 12.0;
        }
    }

    if (profile.sequential_mb_s <= 0.0) {
        profile.sequential_mb_s = 80.0;
    }
    if (profile.random_seek_ms <= 0.0) {
        profile.random_seek_ms = 12.0;
    }

    profile.calibrated = true;
    return profile;
}

DeviceProfileCache& DeviceProfileCache::instance() {
    static DeviceProfileCache cache;
    return cache;
}

DeviceProfile DeviceProfileCache::getOrCalibrate(
    int fd,
    uint64_t file_size,
    const DeviceCalibrationConfig& config
) {
    const DeviceKey key = makeDeviceKey(fd);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = cache_.find(key);
        if (it != cache_.end()) {
            return it->second;
        }
    }

    const DeviceProfile profile =
        calibrateDeviceProfile(fd, file_size, config);

    // A fallback from a small file is not a measured property of the device.
    // Do not poison the device-wide cache; a later large competition file must
    // still be able to perform the real calibration.
    if (profile.calibrated) {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_[key] = profile;
    }

    return profile;
}

void DeviceProfileCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
}

} // namespace erwt3d
