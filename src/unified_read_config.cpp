#include "erwt3d/unified_read_config.hpp"

#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/vfs.h>

namespace erwt3d {

IOProfileType resolveIOProfile(
    IOProfileType requested,
    const std::string& inputPath,
    std::string& reason,
    std::string& filesystemType,
    bool& wslDetected)
{
    wslDetected = detectWSL();

    {
        struct statfs sfs{};
        if (statfs(inputPath.c_str(), &sfs) == 0) {
            switch (sfs.f_type) {
                case 0xEF53: filesystemType = "ext4"; break;
                case 0x58465342: filesystemType = "xfs"; break;
                case 0x4D44: filesystemType = "msdos"; break;
                case 0x6969: filesystemType = "nfs"; break;
                case 0x01021994: filesystemType = "tmpfs"; break;
                case 0x9123683E: filesystemType = "btrfs"; break;
                case 0x5346544E: filesystemType = "ntfs"; break;
                case 0x6165676C: filesystemType = "pstore"; break;
                default: {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "0x%lx",
                             static_cast<unsigned long>(sfs.f_type));
                    filesystemType = buf;
                    break;
                }
            }
        }
    }

    if (requested != IOProfileType::Auto) {
        reason = "user-specified";
        return requested;
    }

    if (wslDetected) {
        bool isNativeExt4 = (filesystemType == "ext4" || filesystemType == "xfs" ||
                             filesystemType == "btrfs");
        bool isMntPath = (inputPath.find("/mnt/") == 0);

        if (isNativeExt4 && !isMntPath) {
            reason = "auto-wsl-native-ext4";
            return IOProfileType::WSL_SSD;
        }
        if (isMntPath) {
            reason = "auto-wsl-mnt-fallback-hdd";
            return IOProfileType::HDD;
        }
    }

    reason = "auto-default-hdd";
    return IOProfileType::HDD;
}

UnifiedReadConfig makeUnifiedConfig(
    IOProfileType requested,
    const std::string& inputPath,
    int threads,
    uint64_t memoryLimitMib,
    uint64_t readWindowMb)
{
    UnifiedReadConfig cfg;
    cfg.threads = threads;
    cfg.memory_limit_mib = memoryLimitMib;

    cfg.io_profile = resolveIOProfile(requested, inputPath,
        cfg.resolved_profile_reason, cfg.filesystem_type, cfg.wsl_detected);

    constexpr uint64_t MiB = 1024ULL * 1024ULL;

    const bool isSSD = (cfg.io_profile == IOProfileType::SSD ||
                        cfg.io_profile == IOProfileType::WSL_SSD);

    cfg.hdd.seek_ms = isSSD ? 0.1 : 10.0;
    cfg.hdd.sequential_mb_s = isSSD ? 3000.0 : 250.0;
    cfg.hdd.read_window_bytes = readWindowMb > 0
        ? std::min<uint64_t>(readWindowMb * MiB, 128ULL * MiB)
        : (isSSD ? 4ULL * MiB : 128ULL * MiB);
    cfg.hdd.max_gap_bytes = isSSD ? 64ULL * 1024 : 8ULL * MiB;

    cfg.ssd.read_threads = 4;
    cfg.ssd.decode_threads = std::max(1, threads);
    cfg.ssd.read_window_bytes = 4ULL * MiB;
    cfg.ssd.max_gap_bytes = 64ULL * 1024;
    cfg.ssd.queue_depth = 8;
    cfg.ssd.buffer_pool_bytes = 512ULL * MiB;
    cfg.ssd.fuse_decode_scatter = true;
    cfg.ssd.use_fadvise = true;
    cfg.ssd.pin_workers = false;

    cfg.writer.writer_threads = 2;
    cfg.writer.queue_high_water_bytes = 768ULL * MiB;
    cfg.writer.queue_low_water_bytes = 256ULL * MiB;
    cfg.writer.use_ftruncate = true;
    cfg.writer.use_posix_fallocate = false;
    cfg.writer.write_whole_slice = true;

    return cfg;
}

} // namespace erwt3d
