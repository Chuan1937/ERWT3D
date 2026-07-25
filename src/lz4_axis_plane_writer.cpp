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
    Lz4AxisPlaneWriterStats* stats,
    uint64_t memoryLimitMiB
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

    constexpr uint64_t MiB = 1ULL << 20;
    constexpr uint64_t kLegacyInputSlabBytes = 512ULL << 20;
    constexpr uint64_t kWholePlaneHeadroom = 512ULL << 20;
    constexpr uint64_t kMaxSinglePlaneBytes = 128ULL << 20;
    const uint64_t memoryBudgetBytes =
        memoryLimitMiB == 0 ||
                memoryLimitMiB >
                    std::numeric_limits<uint64_t>::max() / MiB
            ? std::numeric_limits<uint64_t>::max()
            : memoryLimitMiB * MiB;
    uint64_t wholePlaneBytes = 0;
    const bool wholePlaneFits =
        checkedAdd(rawBytes, kWholePlaneHeadroom, wholePlaneBytes) &&
        memoryBudgetBytes >= wholePlaneBytes;
    const bool singleRecord =
        shape.plane_bytes <= kMaxSinglePlaneBytes &&
        (memoryLimitMiB == 0 || wholePlaneFits);

    uint64_t slabBudgetBytes = kLegacyInputSlabBytes;
    if (!singleRecord && memoryLimitMiB != 0) {
        slabBudgetBytes = std::max<uint64_t>(
            inputSlabBytes,
            memoryBudgetBytes - memoryBudgetBytes / 4);
    }
    uint64_t rowBytes = 0;
    if (!checkedMul(shape.dim_b, sizeof(float), rowBytes) ||
        rowBytes == 0) {
        return false;
    }
    const uint64_t maxRowsByCodec =
        static_cast<uint64_t>(INT_MAX) / rowBytes;
    if (maxRowsByCodec == 0) return false;
    const uint64_t requestedRows =
        singleRecord
            ? nx
            : std::max<uint64_t>(
                  1,
                  static_cast<uint64_t>(chunkElements) /
                      shape.dim_b);

    const uint64_t slabRows64 = std::max<uint64_t>(1, std::min<uint64_t>(
        std::min({
            nx,
            maxRowsByCodec,
            requestedRows}),
        std::max<uint64_t>(1, slabBudgetBytes / inputSlabBytes)));

    const uint32_t chunksPerPlane = singleRecord ? 1u :
        static_cast<uint32_t>((nx + slabRows64 - 1) / slabRows64);

    const uint64_t totalChunks64 = shape.plane_count * static_cast<uint64_t>(chunksPerPlane);
    if (totalChunks64 > std::numeric_limits<size_t>::max() / sizeof(AxisPlaneIndexEntry)) return false;
    uint64_t indexBytes = totalChunks64 * sizeof(AxisPlaneIndexEntry);
    uint64_t dataOffset = sizeof(AxisPlaneHeader) + indexBytes;
    if (dataOffset < sizeof(AxisPlaneHeader)) return false;

    std::vector<AxisPlaneIndexEntry> index(static_cast<size_t>(totalChunks64));

    AxisPlaneHeader header {};
    initAxisPlaneHeader(header);
    header.axis = static_cast<uint8_t>(axis);
    header.compression = AXISPLANE_COMPRESSION_LZ4;
    header.nx = nx; header.ny = ny; header.nz = nz;
    header.plane_count = shape.plane_count;
    header.plane_elements = shape.plane_elements;
    header.index_offset = sizeof(AxisPlaneHeader);
    header.data_offset = dataOffset;
    header.chunk_rows = singleRecord ? static_cast<uint32_t>(nx) : static_cast<uint32_t>(slabRows64);
    header.chunks_per_plane = chunksPerPlane;
    header.total_chunks = totalChunks64;
    if (!checkedMul(header.chunk_rows, shape.dim_b, header.chunk_raw_bytes) ||
        !checkedMul(header.chunk_raw_bytes, sizeof(float), header.chunk_raw_bytes))
        return false;

    if (!pwriteAll(outFd.get(), &header, sizeof(header), 0) ||
        !pwriteAll(outFd.get(), index.data(), static_cast<size_t>(indexBytes), header.index_offset))
        return false;

    posix_fadvise(rawFd.get(), 0, 0, POSIX_FADV_SEQUENTIAL);

    std::vector<float> slab;
    const size_t workerCount = static_cast<size_t>(std::max(1, std::min(threads, 64)));
    ThreadPool pool(workerCount);

    uint64_t payloadCursor = dataOffset;
    uint64_t payloadBytes = 0;

    if (singleRecord) {
        // Whole-plane mode: scatter slabs to full planes, then encode each plane as one record
        std::vector<std::vector<float>> planeBufs(
            static_cast<size_t>(shape.plane_count));
        for (auto& p : planeBufs)
            p.resize(static_cast<size_t>(shape.plane_elements));

        for (uint64_t x0 = 0; x0 < nx; x0 += slabRows64) {
            const uint64_t rows = std::min(slabRows64, nx - x0);
            uint64_t slabElements = rows * inputSlabElements;
            uint64_t slabBytes = slabElements * sizeof(float);
            slab.resize(static_cast<size_t>(slabElements));
            uint64_t rawOffset = x0 * inputSlabBytes;
            if (!preadAll(rawFd.get(), slab.data(), static_cast<size_t>(slabBytes), rawOffset))
                return false;

            {
                const uint64_t perW = (shape.plane_count + workerCount - 1) / workerCount;
                std::vector<std::future<void>> sf;
                for (size_t w = 0; w < workerCount; ++w) {
                    const uint64_t p0 = static_cast<uint64_t>(w) * perW;
                    const uint64_t p1 = std::min(p0 + perW, shape.plane_count);
                    if (p0 >= p1) break;
                    sf.push_back(pool.submit([&, p0, p1, rows, x0]() {
                        for (uint64_t p = p0; p < p1; ++p) {
                            float* dst = planeBufs[static_cast<size_t>(p)].data();
                            if (axis == PlaneAxis::Y) {
                                for (uint64_t lx = 0; lx < rows; ++lx) {
                                    const float* src = slab.data() + (lx * ny + p) * nz;
                                    std::copy_n(src, nz, dst + (x0 + lx) * nz);
                                }
                            } else {
                                for (uint64_t lx = 0; lx < rows; ++lx) {
                                    const float* xs = slab.data() + lx * ny * nz;
                                    for (uint64_t y = 0; y < ny; ++y)
                                        dst[(x0 + lx) * ny + y] = xs[y * nz + p];
                                }
                            }
                        }
                    }));
                }
                for (auto& f : sf) f.get();
            }
        }

        // Encode each complete plane
        const uint64_t inFlight = std::max<uint64_t>(1, static_cast<uint64_t>(workerCount) * 2);
        for (uint64_t firstPlane = 0; firstPlane < shape.plane_count; firstPlane += inFlight) {
            const uint64_t endPlane = std::min(shape.plane_count, firstPlane + inFlight);
            std::vector<std::future<std::vector<uint8_t>>> futures;
            for (uint64_t p = firstPlane; p < endPlane; ++p) {
                futures.push_back(pool.submit([&, p] {
                    const float* plane = planeBufs[static_cast<size_t>(p)].data();
                    const int rawSize = static_cast<int>(shape.plane_bytes);
                    const int bound = LZ4_compressBound(rawSize);
                    std::vector<uint8_t> comp(static_cast<size_t>(bound));
                    const int cs = LZ4_compress_default(
                        reinterpret_cast<const char*>(plane),
                        reinterpret_cast<char*>(comp.data()), rawSize, bound);
                    if (cs <= 0) return std::vector<uint8_t>{};
                    comp.resize(static_cast<size_t>(cs));
                    return comp;
                }));
            }
            for (uint64_t p = firstPlane; p < endPlane; ++p) {
                auto comp = futures[static_cast<size_t>(p - firstPlane)].get();
                if (comp.empty()) return false;
                index[static_cast<size_t>(p)].offset = payloadCursor;
                index[static_cast<size_t>(p)].compressed_size = static_cast<uint32_t>(comp.size());
                index[static_cast<size_t>(p)].raw_size = static_cast<uint32_t>(shape.plane_bytes);
                if (!pwriteAll(outFd.get(), comp.data(), comp.size(), payloadCursor))
                    return false;
                payloadCursor += comp.size();
                payloadBytes += comp.size();
            }
        }
    } else {
        // Memory-bounded mode. Each source X slab is read once, then its Y
        // or Z fragments are compressed independently. A plane's chunk index
        // stays in X order, so the reader decodes directly into the output.
        for (uint32_t chunk = 0;
             chunk < chunksPerPlane;
             ++chunk) {
            const uint64_t x0 =
                static_cast<uint64_t>(chunk) * slabRows64;
            const uint64_t rows = std::min(slabRows64, nx - x0);
            uint64_t slabElements = 0;
            uint64_t slabBytes = 0;
            if (!checkedMul(rows, inputSlabElements, slabElements) ||
                !checkedMul(slabElements, sizeof(float), slabBytes) ||
                slabElements > std::numeric_limits<size_t>::max() ||
                slabBytes > std::numeric_limits<size_t>::max()) {
                return false;
            }
            slab.resize(static_cast<size_t>(slabElements));
            if (!preadAll(
                    rawFd.get(),
                    slab.data(),
                    static_cast<size_t>(slabBytes),
                    x0 * inputSlabBytes)) {
                return false;
            }

            uint64_t chunkElements64 = 0;
            uint64_t chunkBytes64 = 0;
            if (!checkedMul(rows, shape.dim_b, chunkElements64) ||
                !checkedMul(
                    chunkElements64,
                    sizeof(float),
                    chunkBytes64) ||
                chunkElements64 >
                    std::numeric_limits<size_t>::max() ||
                chunkBytes64 >
                    static_cast<uint64_t>(INT_MAX)) {
                return false;
            }
            const int rawChunkBytes =
                static_cast<int>(chunkBytes64);
            const uint64_t inFlight = std::max<uint64_t>(
                1,
                static_cast<uint64_t>(workerCount) * 2);

            for (uint64_t firstPlane = 0;
                 firstPlane < shape.plane_count;
                 firstPlane += inFlight) {
                const uint64_t endPlane = std::min(
                    shape.plane_count,
                    firstPlane + inFlight);
                std::vector<std::future<std::vector<uint8_t>>> futures;
                futures.reserve(
                    static_cast<size_t>(endPlane - firstPlane));
                for (uint64_t p = firstPlane;
                     p < endPlane;
                     ++p) {
                    futures.push_back(pool.submit(
                        [&, p, rows, rawChunkBytes] {
                            std::vector<float> chunkData(
                                static_cast<size_t>(
                                    rows * shape.dim_b));
                            if (axis == PlaneAxis::Y) {
                                for (uint64_t lx = 0;
                                     lx < rows;
                                     ++lx) {
                                    const float* src =
                                        slab.data() +
                                        (lx * ny + p) * nz;
                                    std::copy_n(
                                        src,
                                        nz,
                                        chunkData.data() + lx * nz);
                                }
                            } else {
                                for (uint64_t lx = 0;
                                     lx < rows;
                                     ++lx) {
                                    const float* xs =
                                        slab.data() + lx * ny * nz;
                                    for (uint64_t y = 0;
                                         y < ny;
                                         ++y) {
                                        chunkData[lx * ny + y] =
                                            xs[y * nz + p];
                                    }
                                }
                            }
                            const int bound =
                                LZ4_compressBound(rawChunkBytes);
                            std::vector<uint8_t> compressed(
                                static_cast<size_t>(bound));
                            const int compressedBytes =
                                LZ4_compress_default(
                                    reinterpret_cast<const char*>(
                                        chunkData.data()),
                                    reinterpret_cast<char*>(
                                        compressed.data()),
                                    rawChunkBytes,
                                    bound);
                            if (compressedBytes <= 0) {
                                return std::vector<uint8_t>{};
                            }
                            compressed.resize(
                                static_cast<size_t>(
                                    compressedBytes));
                            return compressed;
                        }));
                }

                for (uint64_t p = firstPlane;
                     p < endPlane;
                     ++p) {
                    auto compressed =
                        futures[static_cast<size_t>(
                            p - firstPlane)].get();
                    if (compressed.empty()) return false;
                    const uint64_t indexId =
                        p * chunksPerPlane + chunk;
                    auto& entry =
                        index[static_cast<size_t>(indexId)];
                    entry.offset = payloadCursor;
                    entry.compressed_size =
                        static_cast<uint32_t>(compressed.size());
                    entry.raw_size =
                        static_cast<uint32_t>(rawChunkBytes);
                    if (!pwriteAll(
                            outFd.get(),
                            compressed.data(),
                            compressed.size(),
                            payloadCursor)) {
                        return false;
                    }
                    payloadCursor += compressed.size();
                    payloadBytes += compressed.size();
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
    Lz4AxisPlaneWriterStats* stats,
    uint64_t memoryLimitMiB
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
            stats,
            memoryLimitMiB);
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
    (void)memoryLimitMiB;
    return false;
#endif
}

}  // namespace erwt3d
