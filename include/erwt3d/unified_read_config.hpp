#pragma once

#include "io_profile.hpp"
#include "sb_hdd.hpp"
#include "ssd/ssd_config.hpp"

#include <cstdint>
#include <string>

namespace erwt3d {

struct UnifiedReadConfig {
    IOProfileType io_profile = IOProfileType::Auto;
    int threads = 8;
    uint64_t memory_limit_mib = 0;

    HDDReadWindowConfig hdd;
    SSDReadConfig ssd;
    SSDWriterConfig writer;

    std::string resolved_profile_reason;
    std::string filesystem_type;
    bool wsl_detected = false;
};

UnifiedReadConfig makeUnifiedConfig(
    IOProfileType requested,
    const std::string& inputPath,
    int threads,
    uint64_t memoryLimitMib,
    uint64_t readWindowMb);

IOProfileType resolveIOProfile(
    IOProfileType requested,
    const std::string& inputPath,
    std::string& reason,
    std::string& filesystemType,
    bool& wslDetected);

} // namespace erwt3d
