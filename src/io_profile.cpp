#include "erwt3d/io_profile.hpp"

#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <sys/vfs.h>

namespace erwt3d {

const char* ioProfileTypeName(IOProfileType t) {
    switch (t) {
        case IOProfileType::Auto:    return "auto";
        case IOProfileType::HDD:     return "hdd";
        case IOProfileType::SSD:     return "ssd";
        case IOProfileType::WSL_SSD: return "wsl-ssd";
    }
    return "unknown";
}

IOProfileType parseIOProfileType(const std::string& s) {
    if (s == "auto")     return IOProfileType::Auto;
    if (s == "hdd")      return IOProfileType::HDD;
    if (s == "ssd")      return IOProfileType::SSD;
    if (s == "wsl-ssd")  return IOProfileType::WSL_SSD;
    return IOProfileType::Auto;
}

bool detectWSL() {
    std::ifstream osRelease("/proc/sys/kernel/osrelease");
    if (!osRelease) return false;
    std::string line;
    std::getline(osRelease, line);
    return line.find("microsoft") != std::string::npos ||
           line.find("WSL")      != std::string::npos;
}

bool detectRotationalFromPath(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return true;
    return false;
}

} // namespace erwt3d
