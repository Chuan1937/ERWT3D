#include "erwt3d/lz4_axis_plane_writer.hpp"
#include "erwt3d/raw_layout.hpp"
#include "erwt3d/thread_pool.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <future>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#ifdef ERWT3D_HAVE_LZ4
#include <lz4.h>
#endif

namespace erwt3d {
namespace {

class ScopedFd {
public:
    explicit ScopedFd(int fd = -1) noexcept : fd_(fd) {}
    ~ScopedFd() {
        if (fd_ >= 0) close(fd_);
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    int get() const noexcept { return fd_; }

private:
    int fd_;
};

class UnlinkUnlessCommitted {
public:
    explicit UnlinkUnlessCommitted(std::string path)
        : path_(std::move(path)) {}

    ~UnlinkUnlessCommitted() {
        if (!committed_) unlink(path_.c_str());
    }

    void commit() noexcept { committed_ = true; }

private:
    std::string path_;
    bool committed_ = false;
};

bool checkedAdd(uint64_t a, uint64_t b, uint64_t& result) {
    if (a > std::numeric_limits<uint64_t>::max() - b) return false;
    result = a + b;
    return true;
}

bool checkedMul(uint64_t a, uint64_t b, uint64_t& result) {
    if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a) return false;
    result = a * b;
    return true;
}

bool validIoRange(uint64_t offset, size_t bytes) {
    const uint64_t maxOffset =
        static_cast<uint64_t>(std::numeric_limits<off_t>::max());
    return offset <= maxOffset &&
           static_cast<uint64_t>(bytes) <= maxOffset - offset;
}

bool preadAll(int fd, void* buffer, size_t bytes, uint64_t offset) {
    if (!validIoRange(offset, bytes)) return false;

    auto* dst = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    while (done < bytes) {
        const size_t request = std::min(
            bytes - done,
            static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t n = pread(
            fd, dst + done, request, static_cast<off_t>(offset + done));
        if (n > 0) {
            done += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool pwriteAll(int fd, const void* buffer, size_t bytes, uint64_t offset) {
    if (!validIoRange(offset, bytes)) return false;

    const auto* src = static_cast<const uint8_t*>(buffer);
    size_t done = 0;
    while (done < bytes) {
        const size_t request = std::min(
            bytes - done,
            static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t n = pwrite(
            fd, src + done, request, static_cast<off_t>(offset + done));
        if (n > 0) {
            done += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool getFileSize(int fd, uint64_t& bytes) {
    struct stat st {};
    if (fstat(fd, &st) != 0 || st.st_size < 0) return false;
    bytes = static_cast<uint64_t>(st.st_size);
    return true;
}

#ifdef ERWT3D_HAVE_LZ4

struct CompressedChunk {
    uint64_t plane = 0;
    uint32_t chunk = 0;
    uint32_t rawBytes = 0;
    std::vector<char> compressed;
    bool ok = false;
};

bool writeLz4YZSidecar(
    const std::string& rawPath,
    const std::string& mainPath,
    PlaneAxis axis,
    uint64_t nx,
    uint64_t ny,
    uint64_t nz,
    uint32_t chunkElements,
    double storageBudget,
    int threads,
    Lz4AxisPlaneWriterStats* stats
) {
    if (axis != PlaneAxis::Y && axis != PlaneAxis::Z) return false;
    if (nx == 0 || ny == 0 || nz == 0 || storageBudget <= 0.0) return false;

    const AxisPlaneShape shape = makeAxisPlaneShape(axis, nx, ny, nz);
    if (shape.plane_count > std::numeric_limits<uint32_t>::max()) return false;

    uint64_t rawElements = 0;
    uint64_t rawBytes = 0;
    if (!checkedMul(nx, ny, rawElements) ||
        !checkedMul(rawElements, nz, rawElements) ||
        !checkedMul(rawElements, sizeof(float), rawBytes)) {
        return false;
    }

    ScopedFd rawFd(open(rawPath.c_str(), O_RDONLY | O_CLOEXEC));
    if (rawFd.get() < 0) return false;

    uint64_t actualRawBytes = 0;
    if (!getFileSize(rawFd.get(), actualRawBytes) ||
        actualRawBytes != rawBytes) {
        return false;
    }

    ScopedFd mainFd(open(mainPath.c_str(), O_RDONLY | O_CLOEXEC));
    if (mainFd.get() < 0) return false;

    uint64_t mainBytes = 0;
    if (!getFileSize(mainFd.get(), mainBytes)) return false;

    const std::string sidecarPath = axisPlaneSidecarPath(mainPath, axis);
    UnlinkUnlessCommitted cleanup(sidecarPath);
    ScopedFd outFd(open(
        sidecarPath.c_str(),
        O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC,
        0644));
    if (outFd.get() < 0) return false;

    // Y and Z planes both use X as their row dimension.  One input X slab
    // is contiguous, so processing a bounded group of X rows reads the raw
    // file exactly once and avoids millions of tiny row reads.
    uint64_t inputSlabElements = 0;
    uint64_t inputSlabBytes = 0;
    if (!checkedMul(ny, nz, inputSlabElements) ||
        !checkedMul(inputSlabElements, sizeof(float), inputSlabBytes) ||
        inputSlabBytes == 0) {
        return false;
    }

    constexpr uint64_t kMaxInputSlabBytes = 512ULL << 20;
    const uint64_t maxRowsByWorkspace =
        std::max<uint64_t>(1, kMaxInputSlabBytes / inputSlabBytes);
    const uint64_t maxChunkFloats =
        static_cast<uint64_t>(INT_MAX) / sizeof(float);
    const uint64_t maxRowsByLz4 = maxChunkFloats / shape.dim_b;
    if (maxRowsByLz4 == 0) return false;

    const uint64_t requestedRows = std::max<uint64_t>(
        1, static_cast<uint64_t>(chunkElements) / shape.dim_b);
    const uint64_t chunkRows64 = std::min(
        {nx, requestedRows, maxRowsByWorkspace, maxRowsByLz4,
         static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())});
    if (chunkRows64 == 0) return false;

    const uint32_t chunkRows = static_cast<uint32_t>(chunkRows64);
    const uint64_t chunksPerPlane64 = (nx + chunkRows64 - 1) / chunkRows64;
    if (chunksPerPlane64 == 0 ||
        chunksPerPlane64 > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    const uint32_t chunksPerPlane =
        static_cast<uint32_t>(chunksPerPlane64);

    uint64_t totalChunks = 0;
    uint64_t indexBytes = 0;
    uint64_t dataOffset = 0;
    if (!checkedMul(shape.plane_count, chunksPerPlane64, totalChunks) ||
        totalChunks > std::numeric_limits<size_t>::max() /
                          sizeof(AxisPlaneIndexEntry) ||
        !checkedMul(totalChunks, sizeof(AxisPlaneIndexEntry), indexBytes) ||
        !checkedAdd(sizeof(AxisPlaneHeader), indexBytes, dataOffset)) {
        return false;
    }

    std::vector<AxisPlaneIndexEntry> index(
        static_cast<size_t>(totalChunks));

    AxisPlaneHeader header {};
    initAxisPlaneHeader(header);
    header.axis = static_cast<uint8_t>(axis);
    header.compression = AXISPLANE_COMPRESSION_LZ4;
    header.nx = nx;
    header.ny = ny;
    header.nz = nz;
    header.plane_count = shape.plane_count;
    header.plane_elements = shape.plane_elements;
    header.index_offset = sizeof(AxisPlaneHeader);
    header.data_offset = dataOffset;
    header.chunk_rows = chunkRows;
    header.chunks_per_plane = chunksPerPlane;
    header.total_chunks = totalChunks;
    if (!checkedMul(chunkRows64, shape.dim_b, header.chunk_raw_bytes) ||
        !checkedMul(
            header.chunk_raw_bytes, sizeof(float), header.chunk_raw_bytes)) {
        return false;
    }

    // Materialize the fixed layout before payload writes.  Every later write
    // is positional, so no thread or final header update can disturb a shared
    // file cursor.
    if (!pwriteAll(outFd.get(), &header, sizeof(header), 0) ||
        !pwriteAll(
            outFd.get(), index.data(), static_cast<size_t>(indexBytes),
            header.index_offset)) {
        return false;
    }

    posix_fadvise(rawFd.get(), 0, 0, POSIX_FADV_SEQUENTIAL);

    std::vector<float> slab;
    const size_t workerCount = static_cast<size_t>(
        std::max(1, std::min(threads, 64)));
    ThreadPool pool(workerCount);
    const uint64_t inFlightLimit =
        std::max<uint64_t>(1, static_cast<uint64_t>(workerCount) * 2);

    uint64_t payloadCursor = dataOffset;
    uint64_t payloadBytes = 0;

    for (uint64_t x0 = 0, chunkOrdinal64 = 0;
         x0 < nx;
         x0 += chunkRows64, ++chunkOrdinal64) {
        const uint64_t rows = std::min<uint64_t>(chunkRows64, nx - x0);
        uint64_t slabElements = 0;
        uint64_t slabBytes64 = 0;
        if (!checkedMul(rows, inputSlabElements, slabElements) ||
            slabElements > std::numeric_limits<size_t>::max() ||
            !checkedMul(slabElements, sizeof(float), slabBytes64) ||
            slabBytes64 > std::numeric_limits<size_t>::max()) {
            return false;
        }

        slab.resize(static_cast<size_t>(slabElements));
        uint64_t rawOffset = 0;
        if (!checkedMul(x0, inputSlabBytes, rawOffset) ||
            !preadAll(
                rawFd.get(), slab.data(), static_cast<size_t>(slabBytes64),
                rawOffset)) {
            return false;
        }

        const uint32_t chunkOrdinal =
            static_cast<uint32_t>(chunkOrdinal64);

        for (uint64_t firstPlane = 0;
             firstPlane < shape.plane_count;
             firstPlane += inFlightLimit) {
            const uint64_t endPlane = std::min<uint64_t>(
                shape.plane_count, firstPlane + inFlightLimit);
            std::vector<std::future<CompressedChunk>> futures;
            futures.reserve(static_cast<size_t>(endPlane - firstPlane));

            for (uint64_t plane = firstPlane; plane < endPlane; ++plane) {
                futures.push_back(pool.submit(
                    [&, plane, rows, chunkOrdinal]() -> CompressedChunk {
                        CompressedChunk result;
                        result.plane = plane;
                        result.chunk = chunkOrdinal;

                        uint64_t chunkFloatCount = 0;
                        if (!checkedMul(rows, shape.dim_b, chunkFloatCount) ||
                            chunkFloatCount >
                                static_cast<uint64_t>(INT_MAX) /
                                    sizeof(float)) {
                            return result;
                        }

                        std::vector<float> rawChunk(
                            static_cast<size_t>(chunkFloatCount));

                        if (axis == PlaneAxis::Y) {
                            for (uint64_t localX = 0;
                                 localX < rows;
                                 ++localX) {
                                const float* src =
                                    slab.data() +
                                    (localX * ny + plane) * nz;
                                float* dst =
                                    rawChunk.data() + localX * nz;
                                std::copy_n(src, nz, dst);
                            }
                        } else {
                            for (uint64_t localX = 0;
                                 localX < rows;
                                 ++localX) {
                                float* dst =
                                    rawChunk.data() + localX * ny;
                                const float* xSlab =
                                    slab.data() + localX * ny * nz;
                                for (uint64_t y = 0; y < ny; ++y) {
                                    dst[y] = xSlab[y * nz + plane];
                                }
                            }
                        }

                        const int rawSize = static_cast<int>(
                            chunkFloatCount * sizeof(float));
                        const int bound = LZ4_compressBound(rawSize);
                        if (bound <= 0) return result;

                        result.compressed.resize(
                            static_cast<size_t>(bound));
                        const int compressedSize = LZ4_compress_default(
                            reinterpret_cast<const char*>(rawChunk.data()),
                            result.compressed.data(),
                            rawSize,
                            bound);
                        if (compressedSize <= 0) {
                            result.compressed.clear();
                            return result;
                        }

                        result.compressed.resize(
                            static_cast<size_t>(compressedSize));
                        result.rawBytes = static_cast<uint32_t>(rawSize);
                        result.ok = true;
                        return result;
                    }));
            }

            for (auto& future : futures) {
                CompressedChunk result = future.get();
                if (!result.ok ||
                    result.compressed.size() >
                        std::numeric_limits<uint32_t>::max()) {
                    return false;
                }

                uint64_t baseIndex = 0;
                if (!checkedMul(
                        result.plane, chunksPerPlane64, baseIndex) ||
                    baseIndex > totalChunks - 1 - result.chunk) {
                    return false;
                }
                const uint64_t indexPosition = baseIndex + result.chunk;
                AxisPlaneIndexEntry& entry =
                    index[static_cast<size_t>(indexPosition)];
                entry.offset = payloadCursor;
                entry.compressed_size =
                    static_cast<uint32_t>(result.compressed.size());
                entry.raw_size = result.rawBytes;

                if (!pwriteAll(
                        outFd.get(),
                        result.compressed.data(),
                        result.compressed.size(),
                        payloadCursor) ||
                    !checkedAdd(
                        payloadCursor,
                        static_cast<uint64_t>(result.compressed.size()),
                        payloadCursor) ||
                    !checkedAdd(
                        payloadBytes,
                        static_cast<uint64_t>(result.compressed.size()),
                        payloadBytes)) {
                    return false;
                }
            }
        }
    }

    header.total_storage_bytes = payloadBytes;
    if (!pwriteAll(
            outFd.get(), index.data(), static_cast<size_t>(indexBytes),
            header.index_offset) ||
        !pwriteAll(outFd.get(), &header, sizeof(header), 0)) {
        return false;
    }

    uint64_t combinedBytes = 0;
    if (!checkedAdd(mainBytes, payloadCursor, combinedBytes)) return false;
    const double totalStorageRatio =
        static_cast<double>(combinedBytes) / static_cast<double>(rawBytes);
    if (totalStorageRatio > storageBudget) return false;

    cleanup.commit();
    if (stats) {
        stats->axis = axis;
        stats->compression_ratio =
            static_cast<double>(payloadBytes) / static_cast<double>(rawBytes);
        stats->total_storage_ratio = totalStorageRatio;
        stats->sidecar_bytes = payloadCursor;
        stats->plane_count = static_cast<uint32_t>(shape.plane_count);
        stats->written = true;
    }
    return true;
}

#endif  // ERWT3D_HAVE_LZ4

}  // namespace

bool writeLz4AxisPlaneSidecar(
    const std::string& rawPath,
    const std::string& mainPath,
    PlaneAxis axis,
    uint64_t nx,
    uint64_t ny,
    uint64_t nz,
    uint32_t chunkElements,
    double storageBudget,
    int threads,
    Lz4AxisPlaneWriterStats* stats
) {
    if (stats) {
        *stats = Lz4AxisPlaneWriterStats {};
        stats->axis = axis;
    }

#ifdef ERWT3D_HAVE_LZ4
    try {
        if (axis == PlaneAxis::X) {
            if (nx == 0 || ny == 0 || nz == 0) return false;
            const uint64_t requestedRows = std::max<uint64_t>(
                1, static_cast<uint64_t>(chunkElements) / ny);
            const uint32_t chunkZRows = static_cast<uint32_t>(
                std::min<uint64_t>(
                    nz,
                    std::min<uint64_t>(
                        requestedRows,
                        std::numeric_limits<uint32_t>::max())));

            Lz4XpSidecarStats xpStats;
            const bool ok = writeLz4XpSidecar(
                rawPath,
                mainPath,
                nx,
                ny,
                nz,
                1,
                chunkZRows,
                storageBudget,
                false,
                &xpStats);
            if (stats) {
                stats->compression_ratio = xpStats.compression_ratio;
                stats->total_storage_ratio = xpStats.total_storage_ratio;
                stats->sidecar_bytes = xpStats.sidecar_bytes;
                stats->plane_count = xpStats.plane_count;
                stats->written = ok;
            }
            return ok;
        }

        return writeLz4YZSidecar(
            rawPath,
            mainPath,
            axis,
            nx,
            ny,
            nz,
            chunkElements,
            storageBudget,
            threads,
            stats);
    } catch (...) {
        if (stats) stats->written = false;
        return false;
    }
#else
    (void)rawPath;
    (void)mainPath;
    (void)axis;
    (void)nx;
    (void)ny;
    (void)nz;
    (void)chunkElements;
    (void)storageBudget;
    (void)threads;
    return false;
#endif
}

}  // namespace erwt3d
