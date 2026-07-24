#include "erwt3d/rzfp_axis_plane_writer.hpp"
#include "erwt3d/raw_layout.hpp"
#include "erwt3d/rzfp_xplane_writer.hpp"
#include "erwt3d/thread_pool.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#ifdef ERWT3D_HAVE_RZFP
#include <zfp.h>
#endif

namespace erwt3d {
namespace {

class ScopedFd {
public:
    explicit ScopedFd(int fd = -1) noexcept : fd_(fd) {}
    ~ScopedFd() { if (fd_ >= 0) close(fd_); }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    int get() const noexcept { return fd_; }
private:
    int fd_;
};

class UnlinkUnlessCommitted {
public:
    explicit UnlinkUnlessCommitted(std::string path) : path_(std::move(path)) {}
    ~UnlinkUnlessCommitted() { if (!committed_) unlink(path_.c_str()); }
    void commit() noexcept { committed_ = true; }
private:
    std::string path_;
    bool committed_ = false;
};

bool checkedMul(uint64_t a, uint64_t b, uint64_t& result) {
    if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a) return false;
    result = a * b;
    return true;
}

bool checkedAdd(uint64_t a, uint64_t b, uint64_t& result) {
    if (a > std::numeric_limits<uint64_t>::max() - b) return false;
    result = a + b;
    return true;
}

bool validIoRange(uint64_t offset, size_t bytes) {
    const uint64_t maxOff = static_cast<uint64_t>(std::numeric_limits<off_t>::max());
    return offset <= maxOff && static_cast<uint64_t>(bytes) <= maxOff - offset;
}

bool preadAll(int fd, void* buf, size_t bytes, uint64_t offset) {
    if (!validIoRange(offset, bytes)) return false;
    auto* dst = static_cast<uint8_t*>(buf);
    size_t done = 0;
    while (done < bytes) {
        const size_t req = std::min(bytes - done,
            static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t n = pread(fd, dst + done, req, static_cast<off_t>(offset + done));
        if (n > 0) { done += static_cast<size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool pwriteAll(int fd, const void* buf, size_t bytes, uint64_t offset) {
    if (!validIoRange(offset, bytes)) return false;
    const auto* src = static_cast<const uint8_t*>(buf);
    size_t done = 0;
    while (done < bytes) {
        const size_t req = std::min(bytes - done,
            static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t n = pwrite(fd, src + done, req, static_cast<off_t>(offset + done));
        if (n > 0) { done += static_cast<size_t>(n); continue; }
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

#ifdef ERWT3D_HAVE_RZFP

bool writeRzfpYZSidecar(
    const std::string& rawPath,
    const std::string& mainPath,
    PlaneAxis axis,
    const RzfpXPlaneCodecConfig& codecConfig,
    uint64_t nx, uint64_t ny, uint64_t nz,
    int threads,
    RzfpAxisPlaneWriterStats* stats
) {
    if (axis != PlaneAxis::Y && axis != PlaneAxis::Z) return false;
    if (nx == 0 || ny == 0 || nz == 0) return false;

    const AxisPlaneShape shape = makeAxisPlaneShape(axis, nx, ny, nz);

    uint64_t rawElements = 0, rawBytes = 0;
    if (!checkedMul(nx, ny, rawElements) ||
        !checkedMul(rawElements, nz, rawElements) ||
        !checkedMul(rawElements, sizeof(float), rawBytes)) return false;

    ScopedFd rawFd(open(rawPath.c_str(), O_RDONLY | O_CLOEXEC));
    if (rawFd.get() < 0) return false;

    uint64_t actualRawBytes = 0;
    if (!getFileSize(rawFd.get(), actualRawBytes) || actualRawBytes != rawBytes) return false;

    ScopedFd mainFd(open(mainPath.c_str(), O_RDONLY | O_CLOEXEC));
    if (mainFd.get() < 0) return false;

    uint64_t mainBytes = 0;
    if (!getFileSize(mainFd.get(), mainBytes)) return false;

    const std::string sidecarPath = axisPlaneSidecarPath(mainPath, axis);
    UnlinkUnlessCommitted cleanup(sidecarPath);
    ScopedFd outFd(open(sidecarPath.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
    if (outFd.get() < 0) return false;

    // Compute header/index layout
    const uint64_t planeCount = shape.plane_count;
    uint64_t indexBytes = 0, dataOffset = 0;
    if (!checkedMul(planeCount, sizeof(AxisPlaneIndexEntry), indexBytes) ||
        !checkedAdd(sizeof(AxisPlaneHeader), indexBytes, dataOffset)) return false;

    // Reserve header + index area
    if (ftruncate(outFd.get(), static_cast<off_t>(dataOffset)) != 0) return false;

    std::vector<AxisPlaneIndexEntry> index(static_cast<size_t>(planeCount));

    AxisPlaneHeader header{};
    initAxisPlaneHeader(header);
    header.axis = static_cast<uint8_t>(axis);
    header.compression = AXISPLANE_COMPRESSION_RZFP_2D;
    header.nx = nx; header.ny = ny; header.nz = nz;
    header.plane_count = planeCount;
    header.plane_elements = shape.plane_elements;
    header.index_offset = sizeof(AxisPlaneHeader);
    header.data_offset = dataOffset;

    // X-slab read: each slab is nx_in_slab * ny * nz floats, contiguous in raw
    const uint64_t slabX = std::max<uint64_t>(1, std::min<uint64_t>(nx, 32));
    const uint64_t inputSlabElements = ny * nz;
    const uint64_t inputSlabBytes = inputSlabElements * sizeof(float);

    ThreadPool pool(static_cast<size_t>(std::max(1, threads)));
    const uint64_t inFlightLimit = std::max<uint64_t>(1,
        static_cast<uint64_t>(std::max(1, threads)) * 2);

    std::atomic<uint64_t> totalCompressed{0};
    uint64_t payloadCursor = dataOffset;

    // Allocate plane buffers via mmap to avoid physical RAM pressure.
    // Y: 2201*2001*3000*4 = 52.8 GB, Z: 3000*2001*2201*4 = 52.8 GB.
    uint64_t totalPlaneFloats = 0, totalPlaneBytes = 0;
    if (!checkedMul(planeCount, shape.plane_elements, totalPlaneFloats) ||
        !checkedMul(totalPlaneFloats, sizeof(float), totalPlaneBytes)) return false;

    std::string tmpName = sidecarPath + ".buf";
    UnlinkUnlessCommitted bufCleanup(tmpName);
    int bufFd = open(tmpName.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (bufFd < 0) return false;
    if (ftruncate(bufFd, static_cast<off_t>(totalPlaneBytes)) != 0) {
        close(bufFd); return false;
    }
    float* const planeBufBase = static_cast<float*>(
        mmap(nullptr, static_cast<size_t>(totalPlaneBytes), PROT_READ | PROT_WRITE,
             MAP_SHARED, bufFd, 0));
    if (planeBufBase == MAP_FAILED) { close(bufFd); return false; }
    close(bufFd);

    // Single pass through raw file, parallel scatter
    const uint64_t totalSlabs = (nx + slabX - 1) / slabX;
    uint64_t slabIdx = 0;
    posix_fadvise(rawFd.get(), 0, 0, POSIX_FADV_SEQUENTIAL);
    for (uint64_t xStart = 0; xStart < nx; xStart += slabX, ++slabIdx) {
        const uint64_t xEnd = std::min(xStart + slabX, nx);
        const uint64_t rows = xEnd - xStart;

        uint64_t slabElements = 0, slabBytes = 0;
        if (!checkedMul(rows, inputSlabElements, slabElements) ||
            !checkedMul(slabElements, sizeof(float), slabBytes)) return false;

        std::vector<float> slab(static_cast<size_t>(slabElements));
        uint64_t rawOffset = 0;
        if (!checkedMul(xStart, inputSlabBytes, rawOffset) ||
            !preadAll(rawFd.get(), slab.data(), static_cast<size_t>(slabBytes), rawOffset)) {
            munmap(planeBufBase, static_cast<size_t>(totalPlaneBytes));
            return false;
        }

        // Parallel scatter across planes
        {
            const uint64_t perWorker = (planeCount + static_cast<size_t>(std::max(1, threads)) - 1) / static_cast<size_t>(std::max(1, threads));
            std::vector<std::future<void>> sfutures;
            for (size_t w = 0; w < static_cast<size_t>(std::max(1, threads)); ++w) {
                const uint64_t p0 = static_cast<uint64_t>(w) * perWorker;
                const uint64_t p1 = std::min(p0 + perWorker, planeCount);
                if (p0 >= p1) break;
                sfutures.push_back(pool.submit([&, p0, p1, rows]() {
                    for (uint64_t plane = p0; plane < p1; ++plane) {
                        float* dst = planeBufBase + plane * shape.plane_elements;
                        if (axis == PlaneAxis::Y) {
                            for (uint64_t localX = 0; localX < rows; ++localX) {
                                const uint64_t x = xStart + localX;
                                const float* src = slab.data() + (localX * ny + plane) * nz;
                                std::copy_n(src, nz, dst + x * nz);
                            }
                        } else {
                            for (uint64_t localX = 0; localX < rows; ++localX) {
                                const uint64_t x = xStart + localX;
                                const float* xSlab = slab.data() + localX * ny * nz;
                                for (uint64_t y = 0; y < ny; ++y) {
                                    dst[x * ny + y] = xSlab[y * nz + plane];
                                }
                            }
                        }
                    }
                }));
            }
            for (auto& f : sfutures) f.get();
        }
        std::cerr << "\rRZFP " << axisLabel(axis) << " slab " << (slabIdx + 1)
                  << "/" << totalSlabs << " x=" << xStart << ".." << (xEnd - 1)
                  << std::flush;
    }
    std::cerr << std::endl;

    // Encode in parallel
    uint64_t encodeProgress = 0;
    for (uint64_t firstPlane = 0; firstPlane < planeCount; firstPlane += inFlightLimit) {
        const uint64_t endPlane = std::min(planeCount, firstPlane + inFlightLimit);
        std::vector<std::future<std::vector<uint8_t>>> futures;
        futures.reserve(static_cast<size_t>(endPlane - firstPlane));

        for (uint64_t plane = firstPlane; plane < endPlane; ++plane) {
            futures.push_back(pool.submit(
                [&, plane]() -> std::vector<uint8_t> {
                    return encodeXPlane2D(
                        planeBufBase + plane * shape.plane_elements,
                        shape.dim_a,
                        shape.dim_b,
                        codecConfig);
                }));
        }

        for (uint64_t plane = firstPlane; plane < endPlane; ++plane) {
            std::vector<uint8_t> record =
                futures[static_cast<size_t>(plane - firstPlane)].get();
            const uint32_t size = static_cast<uint32_t>(record.size());
            if (size > 512ULL * 1024 * 1024) { munmap(planeBufBase, totalPlaneBytes); return false; }

            index[static_cast<size_t>(plane)].offset = payloadCursor;
            index[static_cast<size_t>(plane)].compressed_size = size;
            index[static_cast<size_t>(plane)].raw_size = 0;

            if (!record.empty()) {
                if (!pwriteAll(outFd.get(), record.data(), record.size(), payloadCursor)) {
                    munmap(planeBufBase, totalPlaneBytes); return false;
                }
            }
            payloadCursor += record.size();
            totalCompressed += record.size();
        }
        encodeProgress += (endPlane - firstPlane);
        std::cerr << "\rRZFP " << axisLabel(axis) << " encode "
                  << encodeProgress << "/" << planeCount << std::flush;
    }
    std::cerr << std::endl;

    munmap(planeBufBase, static_cast<size_t>(totalPlaneBytes));
    pool.waitAll();

    pool.waitAll();

    // Write index
    if (!pwriteAll(outFd.get(), index.data(),
            static_cast<size_t>(planeCount * sizeof(AxisPlaneIndexEntry)),
            sizeof(AxisPlaneHeader))) {
        return false;
    }

    // Write header
    header.total_storage_bytes = totalCompressed.load();
    if (!pwriteAll(outFd.get(), &header, sizeof(header), 0)) return false;

    cleanup.commit();

    if (stats) {
        stats->axis = axis;
        stats->plane_count = planeCount;
        stats->total_raw_bytes = rawBytes;
        stats->total_compressed_bytes = totalCompressed.load();
        stats->compression_ratio = rawBytes > 0
            ? static_cast<double>(totalCompressed.load()) / static_cast<double>(rawBytes) : 0.0;
        stats->sidecar_bytes = payloadCursor;
        stats->written = true;
    }
    return true;
}

#endif // ERWT3D_HAVE_RZFP

} // namespace

bool writeRzfpAxisPlaneSidecar(
    const std::string& rawPath,
    const std::string& mainPath,
    PlaneAxis axis,
    const RzfpXPlaneCodecConfig& codecConfig,
    uint64_t nx, uint64_t ny, uint64_t nz,
    int threads,
    RzfpAxisPlaneWriterStats* stats
) {
    if (stats) { *stats = RzfpAxisPlaneWriterStats{}; stats->axis = axis; }

#ifdef ERWT3D_HAVE_RZFP
    try {
        if (axis == PlaneAxis::X) {
            const std::string sidecarPath = axisPlaneSidecarPath(mainPath, axis);
            RzfpXPlaneWriterStats xStats;
            bool ok = writeXPlaneSidecarFile(rawPath, sidecarPath,
                                               codecConfig, nx, ny, nz, threads, &xStats);
            if (stats) {
                stats->plane_count = xStats.plane_count;
                stats->total_raw_bytes = xStats.total_raw_bytes;
                stats->total_compressed_bytes = xStats.total_compressed_bytes;
                stats->compression_ratio = xStats.compression_ratio;
                stats->sidecar_bytes = xStats.total_compressed_bytes
                    + sizeof(AxisPlaneHeader)
                    + xStats.plane_count * sizeof(AxisPlaneIndexEntry);
                stats->written = ok;
            }
            return ok;
        }

        return writeRzfpYZSidecar(rawPath, mainPath, axis, codecConfig,
                                    nx, ny, nz, threads, stats);
    } catch (...) {
        if (stats) stats->written = false;
        return false;
    }
#else
    (void)rawPath; (void)mainPath; (void)axis; (void)codecConfig;
    (void)nx; (void)ny; (void)nz; (void)threads;
    return false;
#endif
}

} // namespace erwt3d
