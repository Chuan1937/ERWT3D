#include "erwt3d/unified_read_config.hpp"

namespace erwt3d {

IOProfileType resolveIOProfile(
    IOProfileType requested,
    const std::string& inputPath,
    std::string& reason,
    std::string& filesystemType,
    bool& wslDetected)
{
    wslDetected = detectWSL();
    filesystemType = "unknown";

    if (requested != IOProfileType::Auto) {
        reason = "user-specified";
        return requested;
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

    cfg.hdd.seek_ms = 10.0;
    cfg.hdd.sequential_mb_s = 250.0;
    cfg.hdd.read_window_bytes = readWindowMb > 0
        ? std::min<uint64_t>(readWindowMb * MiB, 128ULL * MiB)
        : 128ULL * MiB;
    cfg.hdd.max_gap_bytes = 8ULL * MiB;

    return cfg;
}

} // namespace erwt3d
