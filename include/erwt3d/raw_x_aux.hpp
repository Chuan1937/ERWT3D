#pragma once

#include "format.hpp"
#include <cstdint>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string>
#include <unistd.h>
#include <vector>

namespace erwt3d {

constexpr double RAW_X_AUX_HARD_LIMIT = 1.500;
constexpr double RAW_X_AUX_MAX_RATIO = 1.495;
constexpr double RAW_X_AUX_AUTO_LIMIT = 1.490;
constexpr uint64_t RAW_X_AUX_MIN_RAW_BYTES_FOR_RATIO_CHECK = 10ULL * 1024 * 1024;
constexpr uint32_t RAW_X_AUX_ALIGN = 4096;
constexpr uint64_t RAW_X_AUX_COPY_CHUNK = 256ULL * 1024 * 1024;

enum class RawXAuxMode {
    Auto,
    On,
    Off
};

enum class RawXAuxStatus {
    Disabled,
    Stored,
    AlreadyPresent,
    SkippedStorageBudget,
    Failed
};

struct RawXAuxStats {
    RawXAuxStatus status = RawXAuxStatus::Disabled;
    uint64_t raw_x_aux_offset = 0;
    uint64_t raw_x_aux_bytes = 0;
    uint64_t raw_x_aux_plane_bytes = 0;
    uint32_t raw_x_aux_version = 0;
    double total_storage_ratio = 0.0;
    std::string message;

    bool stored() const {
        return status == RawXAuxStatus::Stored ||
               status == RawXAuxStatus::AlreadyPresent;
    }
};

inline bool isRawXAuxBudgetSkip(const RawXAuxStats& stats) {
    return stats.status == RawXAuxStatus::SkippedStorageBudget;
}

inline void setRawXAuxFailure(RawXAuxStats* stats, const std::string& message) {
    if (!stats) return;
    stats->status = RawXAuxStatus::Failed;
    stats->message = message;
}

struct RawXAuxRegion {
    uint64_t offset = 0;
    uint64_t bytes = 0;
    uint64_t plane_bytes = 0;
    uint32_t version = 0;
};

enum class RawXAuxValidationError {
    None,
    UnsupportedVersion,
    ArithmeticOverflow,
    InvalidPlaneBytes,
    InvalidTotalBytes,
    InvalidAlignment,
    RegionBeforePayloadEnd,
    RegionOutOfFileBounds
};

inline const char* rawXAuxValidationErrorStr(RawXAuxValidationError e) {
    switch (e) {
        case RawXAuxValidationError::None: return "none";
        case RawXAuxValidationError::UnsupportedVersion: return "unsupported version";
        case RawXAuxValidationError::ArithmeticOverflow: return "arithmetic overflow";
        case RawXAuxValidationError::InvalidPlaneBytes: return "plane_bytes != ny*nz*sizeof(float)";
        case RawXAuxValidationError::InvalidTotalBytes: return "bytes != nx*plane_bytes";
        case RawXAuxValidationError::InvalidAlignment: return "offset not 4KB-aligned";
        case RawXAuxValidationError::RegionBeforePayloadEnd: return "region starts before main payload end";
        case RawXAuxValidationError::RegionOutOfFileBounds: return "region exceeds file bounds";
    }
    return "unknown";
}

inline bool checkedMulU64(uint64_t a, uint64_t b, uint64_t& out) {
    if (a != 0 && b > UINT64_MAX / a) return false;
    out = a * b;
    return true;
}

inline bool checkedAddU64(uint64_t a, uint64_t b, uint64_t& out) {
    if (b > UINT64_MAX - a) return false;
    out = a + b;
    return true;
}

inline bool readFullyAt(int fd, void* buffer, uint64_t bytes, uint64_t offset) {
    auto* dst = static_cast<uint8_t*>(buffer);
    uint64_t done = 0;
    while (done < bytes) {
        size_t remaining = static_cast<size_t>(std::min<uint64_t>(bytes - done, static_cast<uint64_t>(SIZE_MAX)));
        ssize_t n = pread(fd, dst + done, remaining, static_cast<off_t>(offset + done));
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        done += static_cast<uint64_t>(n);
    }
    return true;
}

inline bool writeFullyAt(int fd, const void* buffer, uint64_t bytes, uint64_t offset) {
    const auto* src = static_cast<const uint8_t*>(buffer);
    uint64_t done = 0;
    while (done < bytes) {
        size_t remaining = static_cast<size_t>(std::min<uint64_t>(bytes - done, static_cast<uint64_t>(SIZE_MAX)));
        ssize_t n = pwrite(fd, src + done, remaining, static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        done += static_cast<uint64_t>(n);
    }
    return true;
}

class ScopedFd {
public:
    explicit ScopedFd(int fd = -1) noexcept : fd_(fd) {}
    ~ScopedFd() { reset(); }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ScopedFd(ScopedFd&& other) noexcept : fd_(other.release()) {}
    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }
    int release() noexcept { int r = fd_; fd_ = -1; return r; }
    void reset(int newFd = -1) noexcept {
        if (fd_ >= 0) close(fd_);
        fd_ = newFd;
    }
private:
    int fd_ = -1;
};

class FileAppendTransaction {
public:
    FileAppendTransaction(int fd, uint64_t originalSize)
        : fd_(fd), originalSize_(originalSize) {}
    FileAppendTransaction(const FileAppendTransaction&) = delete;
    FileAppendTransaction& operator=(const FileAppendTransaction&) = delete;
    ~FileAppendTransaction() { if (!committed_) rollback(); }
    void commit() noexcept { committed_ = true; }
    bool rollback() noexcept {
        if (fd_ < 0) return false;
        bool ok = true;
        if (ftruncate(fd_, static_cast<off_t>(originalSize_)) != 0) ok = false;
        if (fsync(fd_) != 0) ok = false;
        return ok;
    }
private:
    int fd_;
    uint64_t originalSize_;
    bool committed_ = false;
};

template <typename HeaderType>
class FileAppendTransactionWithHeader {
public:
    FileAppendTransactionWithHeader(int fd, uint64_t originalSize, const HeaderType& originalHeader)
        : fd_(fd), originalSize_(originalSize), originalHeader_(originalHeader) {}
    FileAppendTransactionWithHeader(const FileAppendTransactionWithHeader&) = delete;
    FileAppendTransactionWithHeader& operator=(const FileAppendTransactionWithHeader&) = delete;
    ~FileAppendTransactionWithHeader() { if (!committed_) rollback(); }
    void commit() noexcept { committed_ = true; }
    bool rollback() noexcept {
        if (fd_ < 0) return false;
        bool ok = true;
        if (ftruncate(fd_, static_cast<off_t>(originalSize_)) != 0) ok = false;
        if (!writeFullyAt(fd_, &originalHeader_, sizeof(originalHeader_), 0)) ok = false;
        if (fsync(fd_) != 0) ok = false;
        return ok;
    }
private:
    int fd_ = -1;
    uint64_t originalSize_ = 0;
    HeaderType originalHeader_{};
    bool committed_ = false;
};

inline RawXAuxValidationError validateRawXAuxRegion(
    uint64_t fileSize,
    uint64_t minimumOffset,
    uint64_t nx,
    uint64_t ny,
    uint64_t nz,
    const RawXAuxRegion& region)
{
    if (region.version != RAW_X_AUX_VERSION)
        return RawXAuxValidationError::UnsupportedVersion;

    uint64_t expectedPlaneFloats = 0;
    uint64_t expectedPlaneBytes = 0;
    uint64_t expectedTotalBytes = 0;
    uint64_t regionEnd = 0;

    if (!checkedMulU64(ny, nz, expectedPlaneFloats))
        return RawXAuxValidationError::ArithmeticOverflow;
    if (!checkedMulU64(expectedPlaneFloats, sizeof(float), expectedPlaneBytes))
        return RawXAuxValidationError::ArithmeticOverflow;
    if (!checkedMulU64(nx, expectedPlaneBytes, expectedTotalBytes))
        return RawXAuxValidationError::ArithmeticOverflow;
    if (!checkedAddU64(region.offset, region.bytes, regionEnd))
        return RawXAuxValidationError::ArithmeticOverflow;

    if (region.plane_bytes != expectedPlaneBytes)
        return RawXAuxValidationError::InvalidPlaneBytes;
    if (region.bytes != expectedTotalBytes)
        return RawXAuxValidationError::InvalidTotalBytes;
    if (region.offset % RAW_X_AUX_ALIGN != 0)
        return RawXAuxValidationError::InvalidAlignment;
    if (region.offset < minimumOffset)
        return RawXAuxValidationError::RegionBeforePayloadEnd;
    if (regionEnd > fileSize)
        return RawXAuxValidationError::RegionOutOfFileBounds;

    return RawXAuxValidationError::None;
}

} // namespace erwt3d
