#pragma once

#include "format.hpp"

#include <cstdint>
#include <string>

namespace erwt3d {

enum class OptimizedFileFormat {
    LZ4_ERWT3D,
    RZFP,
    Unknown,
};

OptimizedFileFormat detectOptimizedFileFormat(
    const std::string& path,
    std::string* error = nullptr);

uint64_t getTotalOptimizedStorageBytes(
    const std::string& path,
    uint64_t mainFileBytes,
    const ERWT3DHeader& header);

bool pathsReferToSameFile(
    const std::string& input,
    const std::string& output);

struct ResolvedMemoryLimit {
    std::string mode;
    uint64_t mib = 0;
    bool valid = true;
    std::string error;
};

ResolvedMemoryLimit resolveMemoryLimit(const std::string& value);

}
