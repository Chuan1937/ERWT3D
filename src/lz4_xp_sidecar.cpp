#include "erwt3d/lz4_xp_sidecar.hpp"
#include "erwt3d/format.hpp"
#include "erwt3d/thread_pool.hpp"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <future>
#include <iostream>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifdef ERWT3D_HAVE_LZ4
#include <lz4.h>
#endif

namespace erwt3d {

bool writeLz4XpSidecar(
    const std::string& rawPath,
    const std::string& erwtPath,
    uint64_t nx, uint64_t ny, uint64_t nz,
    uint32_t requestedStride,
    uint32_t chunkZRows,
    double storageBudget,
    bool embed,
    Lz4XpSidecarStats* stats
) {
#ifdef ERWT3D_HAVE_LZ4
    uint64_t rawBytes = nx * ny * nz * sizeof(float);
    uint64_t planeFloats = ny * nz;

    int fdRaw = open(rawPath.c_str(), O_RDONLY);
    if (fdRaw < 0) return false;

    int fdErwt = open(erwtPath.c_str(), O_RDWR);
    if (fdErwt < 0) { close(fdRaw); return false; }

    ERWT3DHeader header;
    if (pread(fdErwt, &header, sizeof(header), 0) != sizeof(header)) {
        close(fdRaw); close(fdErwt); return false;
    }

    struct stat erwtStat;
    fstat(fdErwt, &erwtStat);
    uint64_t mainBytes = erwtStat.st_size;

    posix_fadvise(fdRaw, 0, 0, POSIX_FADV_SEQUENTIAL);

    uint32_t stride = std::max(requestedStride, 1u);
    uint32_t chunksPerPlane = (nz + chunkZRows - 1) / chunkZRows;
    uint32_t planeCount = (nx + stride - 1) / stride;
    uint64_t totalChunks = static_cast<uint64_t>(planeCount) * chunksPerPlane;
    uint64_t chunkRawBytes = ny * static_cast<uint64_t>(chunkZRows) * sizeof(float);

    auto pool = std::make_unique<ThreadPool>(std::min<uint32_t>(8, chunksPerPlane));

    // Determine write target
    int fdXp = -1;
    std::string xpPath;
    uint64_t xpDataStart = 0; // offset in the target file where XP data begins

    if (embed) {
        fdXp = fdErwt;
        xpDataStart = mainBytes;
    } else {
        xpPath = erwtPath + ".xp";
        fdXp = open(xpPath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fdXp < 0) { close(fdRaw); close(fdErwt); return false; }
        xpDataStart = 0;
    }

    // Write XP sidecar header (128 bytes)
    // For embedded: written at mainBytes
    // For sidecar:  written at 0
    XPSidecarHeader xpHdr{};
    std::memcpy(xpHdr.magic, XPSIDECAR_MAGIC, 8);
    xpHdr.version = XPSIDECAR_VERSION;
    xpHdr.nx = nx; xpHdr.ny = ny; xpHdr.nz = nz;
    xpHdr.stride = stride;
    xpHdr.chunk_z_rows = chunkZRows;
    xpHdr.chunks_per_plane = chunksPerPlane;
    xpHdr.plane_count = planeCount;
    xpHdr.plane_floats = ny * nz;
    xpHdr.chunk_raw_bytes = chunkRawBytes;
    xpHdr.total_chunks = totalChunks;
    xpHdr.compression = 1;
    if (pwrite(fdXp, &xpHdr, sizeof(xpHdr), xpDataStart) != sizeof(xpHdr)) {
        if (!embed) close(fdXp);
        close(fdRaw); close(fdErwt); return false;
    }

    // Pre-extend file size for embedded mode
    if (embed) {
        if (ftruncate(fdErwt, mainBytes + sizeof(XPSidecarHeader)) != 0) {
            close(fdRaw); close(fdErwt); return false;
        }
    }

    std::vector<XPChunkIndex> index(totalChunks);
    uint64_t dataOffset = xpDataStart + sizeof(xpHdr);
    uint64_t totalStorageBytes = 0;
    std::vector<float> rawPlane(planeFloats);

    const int compBound = LZ4_compressBound(static_cast<int>(chunkRawBytes));
    std::vector<std::vector<float>> chunkWorkspace(chunksPerPlane,
        std::vector<float>(ny * chunkZRows, 0.0f));
    std::vector<std::vector<uint8_t>> compWorkspace(chunksPerPlane,
        std::vector<uint8_t>(static_cast<size_t>(compBound)));

    for (uint32_t pi = 0; pi < planeCount; ++pi) {
        uint64_t x = static_cast<uint64_t>(pi) * stride;
        if (x >= nx) x = nx - 1;

        uint64_t planeOff = x * planeFloats * sizeof(float);
        ssize_t rd = pread(fdRaw, rawPlane.data(), planeFloats * sizeof(float), planeOff);
        if (rd != static_cast<ssize_t>(planeFloats * sizeof(float))) {
            if (!embed) close(fdXp);
            close(fdRaw); close(fdErwt); return false;
        }
        const float* plane = rawPlane.data();

        using ChunkResult = std::pair<bool, int>;
        std::vector<std::future<ChunkResult>> futures;
        for (uint32_t c = 0; c < chunksPerPlane; ++c) {
            futures.push_back(pool->submit([&, c, ny, plane, chunkZRows, nz]() -> ChunkResult {
                uint64_t zStart = static_cast<uint64_t>(c) * chunkZRows;
                uint64_t zEnd = std::min(zStart + chunkZRows, nz);
                uint64_t rowsInChunk = zEnd - zStart;

                float* dst = chunkWorkspace[c].data();
                std::fill(dst, dst + rowsInChunk * ny, 0.0f);
                for (uint64_t zi = 0; zi < rowsInChunk; ++zi) {
                    uint64_t z = zStart + zi;
                    for (uint64_t y = 0; y < ny; ++y)
                        dst[zi * ny + y] = plane[y * nz + z];
                }

                const char* src = reinterpret_cast<const char*>(dst);
                uint8_t* out = compWorkspace[c].data();
                int cs = LZ4_compress_default(src, reinterpret_cast<char*>(out),
                                              static_cast<int>(chunkRawBytes), compBound);
                if (cs <= 0) return {false, 0};
                return {true, cs};
            }));
        }

        for (uint32_t c = 0; c < chunksPerPlane; ++c) {
            auto [ok, cs] = futures[c].get();
            if (!ok) {
                if (!embed) close(fdXp);
                close(fdRaw); close(fdErwt); return false;
            }

            uint64_t globalChunkIdx = static_cast<uint64_t>(pi) * chunksPerPlane + c;
            index[globalChunkIdx].chunk_offset = dataOffset + totalStorageBytes;
            index[globalChunkIdx].compressed_size = static_cast<uint32_t>(cs);
            index[globalChunkIdx].raw_size = static_cast<uint32_t>(chunkRawBytes);
            totalStorageBytes += static_cast<uint64_t>(cs);

            if (pwrite(fdXp, compWorkspace[c].data(), cs,
                       index[globalChunkIdx].chunk_offset) != cs) {
                if (!embed) close(fdXp);
                close(fdRaw); close(fdErwt); return false;
            }
        }
    }

    close(fdRaw);

    pool->waitAll();
    pool.reset();

    // Write chunk index
    uint64_t indexOffset = dataOffset + totalStorageBytes;
    ssize_t idxBytes = static_cast<ssize_t>(totalChunks * sizeof(XPChunkIndex));
    if (pwrite(fdXp, index.data(), idxBytes, indexOffset) != idxBytes) {
        if (!embed) close(fdXp);
        close(fdErwt); return false;
    }

    // Update XP header with final offsets
    // For embedded mode, chunk_offset values are absolute file offsets
    xpHdr.index_offset = indexOffset;
    xpHdr.total_storage_bytes = totalStorageBytes;
    if (pwrite(fdXp, &xpHdr, sizeof(xpHdr), xpDataStart) != sizeof(xpHdr)) {
        if (!embed) close(fdXp);
        close(fdErwt); return false;
    }

    // Update main file header
    if (embed) {
        header.flags |= FLAG_HAS_XP_SIDECAR | FLAG_HAS_XP_EMBEDDED;
        header.reserved[21] = 1;
        header.reserved[22] = xpDataStart;
        header.reserved[23] = sizeof(xpHdr) + totalStorageBytes + idxBytes;
        pwrite(fdErwt, &header, sizeof(header), 0);
    } else {
        header.flags |= FLAG_HAS_XP_SIDECAR;
        header.reserved[21] = 1;
        pwrite(fdErwt, &header, sizeof(header), 0);
        close(fdXp);
    }

    close(fdErwt);

    if (stats) {
        stats->sidecar_bytes = totalStorageBytes;
        stats->stride = stride;
        stats->plane_count = planeCount;
        stats->compression_ratio = rawBytes > 0 ? static_cast<double>(totalStorageBytes) / rawBytes : 0.0;
        uint64_t totalFileBytes = embed ? (xpDataStart + sizeof(xpHdr) + totalStorageBytes + idxBytes) : (mainBytes + totalStorageBytes);
        stats->total_storage_ratio = rawBytes > 0 ? static_cast<double>(totalFileBytes) / rawBytes : 0.0;
        stats->embedded = embed;
    }

    return true;
#else
    return false;
#endif
}

} // namespace erwt3d
