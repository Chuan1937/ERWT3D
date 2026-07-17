#include "erwt3d/device_profile.hpp"
#include "erwt3d/raw_x_aux.hpp"

#include <algorithm>
#include <cstring>
#include <chrono>
#include <iostream>
#include <random>
#include <sys/stat.h>

namespace erwt3d {

namespace {

using Clock = std::chrono::steady_clock;

static DeviceKey makeDeviceKey(int fd) {
    DeviceKey key;
    struct stat st;
    if (fstat(fd, &st) == 0) {
        key.device_id = static_cast<uint64_t>(st.st_dev);
        key.filesystem_id = 0;
    }
    return key;
}

} // namespace

DeviceProfile calibrateDeviceProfile(
    int fd,
    uint64_t file_size,
    const DeviceCalibrationConfig& config
) {
    DeviceProfile profile;

    if (file_size < config.sequential_region_bytes * 2 ||
        file_size < config.random_probe_bytes * 4) {
        // File too small for meaningful calibration — use conservative defaults
        profile.sequential_mb_s = 80.0;
        profile.random_seek_ms = 12.0;
        profile.calibrated = false;
        return profile;
    }

    struct stat st;
    if (fstat(fd, &st) == 0) {
        profile.device_id = static_cast<uint64_t>(st.st_dev);
    }

    // Sequential I/O: read 3 non-overlapping regions
    {
        const uint64_t regionBytes = config.sequential_region_bytes;
        const uint32_t count = std::min<uint32_t>(config.sequential_region_count, 3);
        const uint64_t stride = file_size / (count + 1);

        std::vector<double> speeds;
        speeds.reserve(count);
        std::vector<uint8_t> buf(regionBytes);

        for (uint32_t i = 0; i < count; ++i) {
            uint64_t offset = stride * (i + 1);
            if (offset + regionBytes > file_size)
                offset = file_size - regionBytes;

            auto t0 = Clock::now();
            bool ok = readFullyAt(fd, buf.data(), regionBytes, offset);
            auto t1 = Clock::now();

            if (ok) {
                double elapsed = std::chrono::duration<double>(t1 - t0).count();
                double mbPerSec = (regionBytes / (1024.0 * 1024.0)) / elapsed;
                speeds.push_back(mbPerSec);
            }

            if (config.evict_after_probe) {
                posix_fadvise(fd, static_cast<off_t>(offset),
                              static_cast<off_t>(regionBytes), POSIX_FADV_DONTNEED);
            }
        }

        if (!speeds.empty()) {
            std::sort(speeds.begin(), speeds.end());
            profile.sequential_mb_s = speeds[speeds.size() / 2]; // median
            double maxSpeed = speeds.back();
            if (maxSpeed > profile.sequential_mb_s * 1.5)
                profile.cached_mb_s = maxSpeed;
        } else {
            profile.sequential_mb_s = 80.0;
        }
    }

    // Random seek probe
    if (config.random_probe_count > 0 && config.random_probe_bytes > 0) {
        const uint64_t probeBytes = config.random_probe_bytes;
        const uint32_t probeCount = config.random_probe_count;
        const uint64_t region = std::min<uint64_t>(file_size - probeBytes, file_size / 2);
        const uint64_t baseOffset = file_size / 4;

        std::mt19937_64 rng(20260511);
        std::uniform_int_distribution<uint64_t> dist(0, region - 1);

        std::vector<uint8_t> probeBuf(probeBytes);
        std::vector<double> seekTimes;
        seekTimes.reserve(probeCount);

        for (uint32_t i = 0; i < probeCount; ++i) {
            uint64_t offset = baseOffset + dist(rng);
            if (offset + probeBytes > file_size)
                offset = file_size - probeBytes;

            auto t0 = Clock::now();
            bool ok = readFullyAt(fd, probeBuf.data(), probeBytes, offset);
            auto t1 = Clock::now();

            if (ok) {
                double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                // Subtract expected transfer time to isolate seek
                double transferMs = (probeBytes / (1024.0 * 1024.0)) /
                                    std::max(profile.sequential_mb_s, 1.0) * 1000.0;
                double seekMs = std::max(0.0, ms - transferMs);
                seekTimes.push_back(seekMs);
            }

            if (config.evict_after_probe) {
                posix_fadvise(fd, static_cast<off_t>(offset),
                              static_cast<off_t>(probeBytes), POSIX_FADV_DONTNEED);
            }
        }

        if (!seekTimes.empty()) {
            std::sort(seekTimes.begin(), seekTimes.end());
            // Trim top and bottom 10%
            size_t trim = seekTimes.size() / 10;
            if (trim * 2 < seekTimes.size()) {
                double sum = 0;
                for (size_t i = trim; i < seekTimes.size() - trim; ++i)
                    sum += seekTimes[i];
                profile.random_seek_ms = sum / (seekTimes.size() - 2 * trim);
            } else {
                profile.random_seek_ms = seekTimes[seekTimes.size() / 2];
            }
        } else {
            profile.random_seek_ms = 12.0;
        }
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
    DeviceKey key = makeDeviceKey(fd);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            return it->second;
        }
    }

    DeviceProfile profile = calibrateDeviceProfile(fd, file_size, config);

    {
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
