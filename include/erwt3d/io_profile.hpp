#pragma once

#include <cstdint>
#include <string>

namespace erwt3d {

enum class IOProfileType {
    Auto,
    HDD,
    SSD,
    WSL_SSD,
};

enum class AccessPattern {
    Random,
    Continuous,
    Mixed,
};

const char* ioProfileTypeName(IOProfileType t);

IOProfileType parseIOProfileType(const std::string& s);

bool detectWSL();

bool detectRotationalFromPath(const std::string& path);

} // namespace erwt3d
