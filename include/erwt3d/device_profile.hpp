#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace erwt3d {

struct DeviceProfile {
    double sequential_mb_s = 0.0;
    double random_seek_ms = 0.0;
    double cached_mb_s = 0.0;
    double minimum_sequential_mb_s = 0.0;
    double maximum_sequential_mb_s = 0.0;

    uint64_t device_id = 0;
    uint64_t filesystem_id = 0;

    bool direct_io_supported = false;
    bool calibrated = false;
    bool cache_contamination_suspected = false;
};

struct DeviceCalibrationConfig {
    uint64_t sequential_region_bytes = 128ULL * 1024 * 1024;
    uint32_t sequential_region_count = 3;
    uint32_t random_probe_count = 64;

    uint64_t random_probe_bytes = 64ULL * 1024;
    bool evict_before_probe = true;
    bool evict_after_probe = true;
    uint32_t warmup_count = 1;
    double minimum_sequential_mb_s = 80.0;
};

DeviceProfile calibrateDeviceProfile(
    int fd,
    uint64_t file_size,
    const DeviceCalibrationConfig& config = {}
);

struct DeviceKey {
    uint64_t device_id = 0;
    uint64_t filesystem_id = 0;

    bool operator==(const DeviceKey& other) const {
        return device_id == other.device_id &&
               filesystem_id == other.filesystem_id;
    }
};

struct DeviceKeyHash {
    std::size_t operator()(const DeviceKey& key) const {
        return std::hash<uint64_t>()(key.device_id) ^
               (std::hash<uint64_t>()(key.filesystem_id) << 1);
    }
};

class DeviceProfileCache {
public:
    static DeviceProfileCache& instance();

    DeviceProfile getOrCalibrate(
        int fd,
        uint64_t file_size,
        const DeviceCalibrationConfig& config = {}
    );

    void clear();

private:
    std::mutex mutex_;
    std::unordered_map<DeviceKey, DeviceProfile, DeviceKeyHash> cache_;
};

enum class CachePolicy {
    StableAuto,
    DeterministicCold,
    WarmAllowed
};

} // namespace erwt3d
