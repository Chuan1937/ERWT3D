#include "erwt3d/embedded_sections.hpp"

#include "erwt3d/format.hpp"
#include "erwt3d/rzfp_format.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#if defined(__linux__)
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#endif
#include <sys/stat.h>
#include <unistd.h>

namespace erwt3d {
namespace {

bool readFullyAt(int fd, void* dst, size_t bytes, uint64_t offset) {
    auto* out = static_cast<uint8_t*>(dst);
    size_t done = 0;
    while (done < bytes) {
        const ssize_t n = pread(fd, out + done, bytes - done,
                                static_cast<off_t>(offset + done));
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return true;
}

bool writeFullyAt(int fd, const void* src, size_t bytes, uint64_t offset) {
    const auto* in = static_cast<const uint8_t*>(src);
    size_t done = 0;
    while (done < bytes) {
        const ssize_t n = pwrite(fd, in + done, bytes - done,
                                 static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

bool addChecked(uint64_t a, uint64_t b, uint64_t& result) {
    if (a > std::numeric_limits<uint64_t>::max() - b) return false;
    result = a + b;
    return true;
}

bool alignChecked(uint64_t value, uint64_t& result) {
    const uint64_t mask = EMBEDDED_SECTION_ALIGNMENT - 1;
    if (value > std::numeric_limits<uint64_t>::max() - mask) return false;
    result = (value + mask) & ~mask;
    return true;
}

bool validType(uint32_t value) {
    switch (static_cast<EmbeddedSectionType>(value)) {
    case EmbeddedSectionType::Lz4AxisPlaneX:
    case EmbeddedSectionType::Lz4AxisPlaneY:
    case EmbeddedSectionType::Lz4AxisPlaneZ:
    case EmbeddedSectionType::RzfpAxisLeafX:
    case EmbeddedSectionType::RzfpAxisLeafY:
    case EmbeddedSectionType::RzfpAxisLeafZ:
        return true;
    }
    return false;
}

enum class CopyMethod {
    Reflink,
    KernelCopy,
    BufferedCopy,
};

bool copyFileRangeEfficient(
    int sourceFd,
    uint64_t sourceBytes,
    int destinationFd,
    uint64_t destinationOffset,
    std::vector<uint8_t>& buffer,
    CopyMethod& method)
{
    uint64_t destinationEnd = 0;
    if (!addChecked(
            destinationOffset,
            sourceBytes,
            destinationEnd) ||
        destinationEnd > static_cast<uint64_t>(
            std::numeric_limits<off_t>::max()) ||
        ftruncate(
            destinationFd,
            static_cast<off_t>(destinationEnd)) != 0) {
        return false;
    }

#if defined(__linux__) && defined(FICLONERANGE)
    file_clone_range clone{};
    clone.src_fd = sourceFd;
    clone.src_offset = 0;
    clone.src_length = sourceBytes;
    clone.dest_offset = destinationOffset;
    if (ioctl(destinationFd, FICLONERANGE, &clone) == 0) {
        method = CopyMethod::Reflink;
        return true;
    }
#endif

    uint64_t copied = 0;
#if defined(__linux__) && defined(SYS_copy_file_range)
    bool kernelCopyAvailable = true;
    off_t sourceOffset = 0;
    off_t outputOffset =
        static_cast<off_t>(destinationOffset);
    while (copied < sourceBytes && kernelCopyAvailable) {
        const size_t chunk = static_cast<size_t>(
            std::min<uint64_t>(
                sourceBytes - copied,
                1ULL << 30));
        const ssize_t n = static_cast<ssize_t>(syscall(
            SYS_copy_file_range,
            sourceFd,
            &sourceOffset,
            destinationFd,
            &outputOffset,
            chunk,
            0));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 &&
            (errno == EXDEV ||
             errno == EINVAL ||
             errno == ENOSYS ||
             errno == EOPNOTSUPP)) {
            kernelCopyAvailable = false;
            break;
        }
        if (n <= 0) {
            kernelCopyAvailable = false;
            break;
        }
        copied += static_cast<uint64_t>(n);
    }
    if (copied == sourceBytes) {
        method = CopyMethod::KernelCopy;
        return true;
    }
#endif

    while (copied < sourceBytes) {
        const size_t chunk = static_cast<size_t>(
            std::min<uint64_t>(
                buffer.size(),
                sourceBytes - copied));
        if (!readFullyAt(
                sourceFd,
                buffer.data(),
                chunk,
                copied) ||
            !writeFullyAt(
                destinationFd,
                buffer.data(),
                chunk,
                destinationOffset + copied)) {
            return false;
        }
        copied += chunk;
    }
    method = CopyMethod::BufferedCopy;
    return true;
}

struct PrimaryHeader {
    enum class Kind { Erwt3d, Rzfp } kind{};
    ERWT3DHeader erwt{};
    RzfpFileHeader rzfp{};
};

bool readPrimaryHeader(int fd, PrimaryHeader& header) {
    std::array<char, 8> magic{};
    if (!readFullyAt(fd, magic.data(), magic.size(), 0)) return false;
    if (std::memcmp(magic.data(), ERWT3D_MAGIC, magic.size()) == 0) {
        header.kind = PrimaryHeader::Kind::Erwt3d;
        return readFullyAt(fd, &header.erwt, sizeof(header.erwt), 0) &&
               validateHeader(header.erwt) &&
               !hasEmbeddedSections(header.erwt);
    }
    if (std::memcmp(magic.data(), RZFP_MAGIC, magic.size()) == 0) {
        header.kind = PrimaryHeader::Kind::Rzfp;
        return readFullyAt(fd, &header.rzfp, sizeof(header.rzfp), 0) &&
               validateRzfpHeader(header.rzfp) &&
               !hasEmbeddedSections(header.rzfp);
    }
    return false;
}

bool commitPrimaryHeader(
    int fd, PrimaryHeader& header,
    uint64_t directoryOffset, uint64_t directoryBytes, uint64_t packageBytes)
{
    if (header.kind == PrimaryHeader::Kind::Erwt3d) {
        header.erwt.flags |= FLAG_HAS_EMBEDDED_SECTIONS;
        header.erwt.reserved[11] = directoryOffset;
        header.erwt.reserved[12] = directoryBytes;
        header.erwt.reserved[13] = packageBytes;
        return writeFullyAt(fd, &header.erwt, sizeof(header.erwt), 0);
    }
    header.rzfp.flags |= FLAG_HAS_EMBEDDED_SECTIONS;
    header.rzfp.reserved[11] = directoryOffset;
    header.rzfp.reserved[12] = directoryBytes;
    header.rzfp.reserved[13] = packageBytes;
    return writeFullyAt(fd, &header.rzfp, sizeof(header.rzfp), 0);
}

bool writePrimaryHeader(int fd, const PrimaryHeader& header) {
    if (header.kind == PrimaryHeader::Kind::Erwt3d) {
        return writeFullyAt(fd, &header.erwt, sizeof(header.erwt), 0);
    }
    return writeFullyAt(fd, &header.rzfp, sizeof(header.rzfp), 0);
}

} // namespace

bool readEmbeddedSectionDirectory(
    int fd,
    uint64_t directoryOffset,
    uint64_t directoryBytes,
    uint64_t fileBytes,
    std::vector<EmbeddedSectionInfo>& sections)
{
    sections.clear();
    if (fd < 0 || directoryBytes < sizeof(EmbeddedSectionDirectoryHeader) ||
        directoryOffset > fileBytes ||
        directoryBytes > fileBytes - directoryOffset) {
        return false;
    }

    EmbeddedSectionDirectoryHeader header{};
    if (!readFullyAt(fd, &header, sizeof(header), directoryOffset) ||
        std::memcmp(header.magic, EMBEDDED_SECTION_MAGIC,
                    sizeof(header.magic)) != 0 ||
        header.version != EMBEDDED_SECTION_VERSION ||
        header.directory_bytes != directoryBytes ||
        header.package_bytes != fileBytes) {
        return false;
    }

    const uint64_t maxEntries =
        (directoryBytes - sizeof(header)) / sizeof(EmbeddedSectionEntry);
    if (header.entry_count == 0 || header.entry_count > maxEntries ||
        sizeof(header) +
                static_cast<uint64_t>(header.entry_count) *
                    sizeof(EmbeddedSectionEntry) !=
            directoryBytes) {
        return false;
    }

    std::vector<EmbeddedSectionEntry> entries(header.entry_count);
    if (!readFullyAt(fd, entries.data(),
                     entries.size() * sizeof(EmbeddedSectionEntry),
                     directoryOffset + sizeof(header))) {
        return false;
    }

    sections.reserve(entries.size());
    for (const auto& entry : entries) {
        if (!validType(entry.type) || entry.bytes == 0 ||
            entry.offset < sizeof(ERWT3DHeader) ||
            entry.offset > directoryOffset ||
            entry.bytes > directoryOffset - entry.offset) {
            sections.clear();
            return false;
        }
        if (findEmbeddedSection(
                sections, static_cast<EmbeddedSectionType>(entry.type))) {
            sections.clear();
            return false;
        }
        sections.push_back({
            static_cast<EmbeddedSectionType>(entry.type),
            entry.offset,
            entry.bytes,
        });
    }
    return true;
}

const EmbeddedSectionInfo* findEmbeddedSection(
    const std::vector<EmbeddedSectionInfo>& sections,
    EmbeddedSectionType type)
{
    const auto it = std::find_if(
        sections.begin(), sections.end(),
        [type](const EmbeddedSectionInfo& section) {
            return section.type == type;
        });
    return it == sections.end() ? nullptr : &*it;
}

bool embedSectionsInPlace(
    const std::string& primaryPath,
    const std::vector<EmbeddedSectionInput>& inputs,
    bool removeSources,
    EmbeddedPackageStats* stats)
{
    if (stats) *stats = {};
    if (inputs.empty() ||
        inputs.size() > std::numeric_limits<uint32_t>::max()) {
        return false;
    }

    int primaryFd = open(primaryPath.c_str(), O_RDWR | O_CLOEXEC);
    if (primaryFd < 0) return false;

    struct stat primaryStat{};
    PrimaryHeader primaryHeader{};
    if (fstat(primaryFd, &primaryStat) != 0 || primaryStat.st_size < 0 ||
        !readPrimaryHeader(primaryFd, primaryHeader)) {
        close(primaryFd);
        return false;
    }
    const uint64_t originalBytes = static_cast<uint64_t>(primaryStat.st_size);
    const PrimaryHeader originalHeader = primaryHeader;

    struct Source {
        EmbeddedSectionInput input;
        int fd = -1;
        uint64_t bytes = 0;
    };
    std::vector<Source> sources;
    sources.reserve(inputs.size());
    bool setupOk = true;
    for (const auto& input : inputs) {
        if (input.path.empty() || input.path == primaryPath) {
            setupOk = false;
            break;
        }
        const uint32_t type = static_cast<uint32_t>(input.type);
        if (!validType(type)) {
            setupOk = false;
            break;
        }
        for (const auto& source : sources) {
            if (source.input.type == input.type) setupOk = false;
        }
        if (!setupOk) break;

        Source source;
        source.input = input;
        source.fd = open(input.path.c_str(), O_RDONLY | O_CLOEXEC);
        struct stat sourceStat{};
        if (source.fd < 0 || fstat(source.fd, &sourceStat) != 0 ||
            sourceStat.st_size <= 0 ||
            (sourceStat.st_dev == primaryStat.st_dev &&
             sourceStat.st_ino == primaryStat.st_ino)) {
            if (source.fd >= 0) close(source.fd);
            setupOk = false;
            break;
        }
        source.bytes = static_cast<uint64_t>(sourceStat.st_size);
        sources.push_back(std::move(source));
    }
    if (!setupOk) {
        for (auto& source : sources) close(source.fd);
        close(primaryFd);
        return false;
    }

    constexpr size_t COPY_BYTES = 16U * 1024U * 1024U;
    std::vector<uint8_t> buffer(COPY_BYTES);
    std::vector<EmbeddedSectionEntry> entries;
    entries.reserve(sources.size());
    uint64_t cursor = originalBytes;
    uint64_t paddingBytes = 0;
    uint64_t sectionBytes = 0;
    uint64_t reflinkBytes = 0;
    uint64_t kernelCopyBytes = 0;
    uint64_t bufferedCopyBytes = 0;
    bool ok = true;

    for (auto& source : sources) {
        uint64_t aligned = 0;
        if (!alignChecked(cursor, aligned)) {
            ok = false;
            break;
        }
        paddingBytes += aligned - cursor;
        cursor = aligned;

        EmbeddedSectionEntry entry{};
        entry.type = static_cast<uint32_t>(source.input.type);
        entry.offset = cursor;
        entry.bytes = source.bytes;
        entries.push_back(entry);

        if (cursor >
            std::numeric_limits<uint64_t>::max() - source.bytes) {
            ok = false;
            break;
        }
        CopyMethod copyMethod = CopyMethod::BufferedCopy;
        if (!copyFileRangeEfficient(
                source.fd,
                source.bytes,
                primaryFd,
                cursor,
                buffer,
                copyMethod)) {
            ok = false;
            break;
        }
        switch (copyMethod) {
        case CopyMethod::Reflink:
            reflinkBytes += source.bytes;
            break;
        case CopyMethod::KernelCopy:
            kernelCopyBytes += source.bytes;
            break;
        case CopyMethod::BufferedCopy:
            bufferedCopyBytes += source.bytes;
            break;
        }
        if (!ok || !addChecked(cursor, source.bytes, cursor) ||
            !addChecked(sectionBytes, source.bytes, sectionBytes)) {
            ok = false;
            break;
        }
    }

    uint64_t directoryOffset = 0;
    uint64_t directoryBytes = 0;
    uint64_t packageBytes = 0;
    if (ok) {
        if (!alignChecked(cursor, directoryOffset)) {
            ok = false;
        } else {
            paddingBytes += directoryOffset - cursor;
            directoryBytes = sizeof(EmbeddedSectionDirectoryHeader) +
                             entries.size() * sizeof(EmbeddedSectionEntry);
            ok = addChecked(directoryOffset, directoryBytes, packageBytes) &&
                 packageBytes <= static_cast<uint64_t>(
                     std::numeric_limits<off_t>::max());
        }
    }

    if (ok) {
        EmbeddedSectionDirectoryHeader directory{};
        std::memcpy(directory.magic, EMBEDDED_SECTION_MAGIC,
                    sizeof(directory.magic));
        directory.version = EMBEDDED_SECTION_VERSION;
        directory.entry_count = static_cast<uint32_t>(entries.size());
        directory.directory_bytes = directoryBytes;
        directory.package_bytes = packageBytes;
        ok = writeFullyAt(primaryFd, &directory, sizeof(directory),
                          directoryOffset) &&
             writeFullyAt(primaryFd, entries.data(),
                          entries.size() * sizeof(EmbeddedSectionEntry),
                          directoryOffset + sizeof(directory)) &&
             ftruncate(primaryFd, static_cast<off_t>(packageBytes)) == 0 &&
             fsync(primaryFd) == 0 &&
             commitPrimaryHeader(primaryFd, primaryHeader, directoryOffset,
                                 directoryBytes, packageBytes) &&
             fsync(primaryFd) == 0;
    }

    if (!ok) {
        const int truncateResult =
            ftruncate(primaryFd, static_cast<off_t>(originalBytes));
        (void)truncateResult;
        (void)writePrimaryHeader(primaryFd, originalHeader);
        (void)fsync(primaryFd);
    }
    for (auto& source : sources) close(source.fd);
    close(primaryFd);

    if (!ok) return false;
    if (removeSources) {
        for (const auto& source : sources) {
            (void)unlink(source.input.path.c_str());
        }
    }
    if (stats) {
        stats->source_bytes = originalBytes;
        stats->section_bytes = sectionBytes;
        stats->padding_bytes = paddingBytes;
        stats->directory_bytes = directoryBytes;
        stats->package_bytes = packageBytes;
        stats->reflink_bytes = reflinkBytes;
        stats->kernel_copy_bytes = kernelCopyBytes;
        stats->buffered_copy_bytes = bufferedCopyBytes;
    }
    return true;
}

bool copyFileEfficient(
    const std::string& sourcePath,
    const std::string& destinationPath,
    EmbeddedPackageStats* stats)
{
    if (stats) *stats = {};
    if (sourcePath.empty() ||
        destinationPath.empty() ||
        sourcePath == destinationPath) {
        return false;
    }

    const int sourceFd = open(
        sourcePath.c_str(),
        O_RDONLY | O_CLOEXEC);
    if (sourceFd < 0) return false;
    struct stat sourceStat{};
    if (fstat(sourceFd, &sourceStat) != 0 ||
        sourceStat.st_size <= 0) {
        close(sourceFd);
        return false;
    }

    const int destinationFd = open(
        destinationPath.c_str(),
        O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC,
        0644);
    if (destinationFd < 0) {
        close(sourceFd);
        return false;
    }

    const uint64_t bytes =
        static_cast<uint64_t>(sourceStat.st_size);
    std::vector<uint8_t> buffer(16U * 1024U * 1024U);
    CopyMethod method = CopyMethod::BufferedCopy;
    const bool ok =
        copyFileRangeEfficient(
            sourceFd,
            bytes,
            destinationFd,
            0,
            buffer,
            method) &&
        fsync(destinationFd) == 0;
    close(destinationFd);
    close(sourceFd);

    if (!ok) {
        (void)unlink(destinationPath.c_str());
        return false;
    }
    if (stats) {
        stats->source_bytes = bytes;
        stats->package_bytes = bytes;
        switch (method) {
        case CopyMethod::Reflink:
            stats->reflink_bytes = bytes;
            break;
        case CopyMethod::KernelCopy:
            stats->kernel_copy_bytes = bytes;
            break;
        case CopyMethod::BufferedCopy:
            stats->buffered_copy_bytes = bytes;
            break;
        }
    }
    return true;
}

} // namespace erwt3d
