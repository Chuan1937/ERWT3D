#include "erwt3d/file_format_detect.hpp"
#include "erwt3d/rzfp_format.hpp"

#include <cstring>
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

} // namespace erwt3d
