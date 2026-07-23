#include "erwt3d/file_format_detect.hpp"
#include "erwt3d/rzfp_format.hpp"
#include "erwt3d/memory_budget.hpp"

#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace erwt3d {

OptimizedFileFormat detectOptimizedFileFormat(
    const std::string& path,
    std::string* error)
{
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (error) {
            *error = "cannot open file: " + path;
        }
        return OptimizedFileFormat::Unknown;
    }

    char magic[8] = {};
    ssize_t rd = pread(fd, magic, 8, 0);
    close(fd);

    if (rd < 8) {
        if (error) {
            *error = "file too short: " + path;
        }
        return OptimizedFileFormat::Unknown;
    }

    if (std::memcmp(magic, ERWT3D_MAGIC, 8) == 0) {
        return OptimizedFileFormat::LZ4_ERWT3D;
    }
    if (std::memcmp(magic, RZFP_MAGIC, 8) == 0) {
        return OptimizedFileFormat::RZFP;
    }

    if (error) {
        *error = "unknown file format magic in: " + path;
    }
    return OptimizedFileFormat::Unknown;
}

uint64_t getTotalOptimizedStorageBytes(
    const std::string& path,
    uint64_t mainFileBytes,
    const ERWT3DHeader& header)
{
    if (hasXPEmbedded(header)) {
        return mainFileBytes;
    }

    if (hasXPSidecar(header) && !hasXPEmbedded(header)) {
        const std::string sidecarPath = path + ".xp";
        struct stat st{};
        if (stat(sidecarPath.c_str(), &st) == 0) {
            return mainFileBytes + static_cast<uint64_t>(st.st_size);
        }
    }

    return mainFileBytes;
}

bool pathsReferToSameFile(
    const std::string& input,
    const std::string& output)
{
    std::error_code ec;

    if (std::filesystem::exists(input, ec) && std::filesystem::exists(output, ec)) {
        return std::filesystem::equivalent(input, output, ec);
    }

    auto weakInput = std::filesystem::weakly_canonical(input, ec);
    if (ec) return false;

    auto outputAbs = std::filesystem::absolute(output, ec);
    if (ec) return false;

    auto weakOutput = std::filesystem::weakly_canonical(outputAbs, ec);
    if (ec) return false;

    return weakInput == weakOutput;
}

static constexpr size_t MIN_MEMORY_MB = 512;

ResolvedMemoryLimit resolveMemoryLimit(const std::string& value)
{
    if (value == "auto" || value == "0") {
        ResolvedMemoryLimit r;
        r.mode = "auto";
        uint64_t memAvail = readLinuxMemAvailableBytes();
        r.mib = static_cast<size_t>(memAvail * 0.70 / (1024 * 1024));
        if (r.mib < MIN_MEMORY_MB) {
            size_t conservative = static_cast<size_t>(memAvail / 2 / (1024 * 1024));
            r.mib = std::max(conservative, MIN_MEMORY_MB);
        }
        return r;
    }

    char* end = nullptr;
    long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (end != value.c_str() + value.size() || parsed <= 0) {
        ResolvedMemoryLimit r;
        r.mode = "explicit";
        r.valid = false;
        r.error = "invalid --memory-limit-mb value: " + value;
        return r;
    }
    size_t mib = static_cast<size_t>(parsed);
    if (mib < MIN_MEMORY_MB) {
        ResolvedMemoryLimit r;
        r.mode = "explicit";
        r.valid = false;
        r.error = "--memory-limit-mb must be at least " +
                  std::to_string(MIN_MEMORY_MB) + " MiB, got " + std::to_string(mib);
        return r;
    }

    ResolvedMemoryLimit r;
    r.mode = "explicit";
    r.mib = mib;
    return r;
}

} // namespace erwt3d
