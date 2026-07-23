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

}
