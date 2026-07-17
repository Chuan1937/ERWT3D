#include "erwt3d/reader.hpp"
#include "erwt3d/format.hpp"
#include "erwt3d/thread_pool.hpp"
#include <iostream>
#include <vector>
#include <cstring>
#include <cmath>
#include <future>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#ifdef ERWT3D_HAVE_LZ4
#include <lz4.h>
#endif

using erwt3d::ERWT3DHeader;
using erwt3d::XPSidecarHeader;
using erwt3d::XPChunkIndex;
using erwt3d::XPSIDECAR_MAGIC;
using erwt3d::XPSIDECAR_VERSION;

// Raw data layout: X-Y-Z row-major (Z varies fastest):
//   offset(x, y, z) = (x * ny + y) * nz + z.
// A full X-plane (fixed x) is therefore contiguous in the raw file.
//
// Streaming sidecar build: for each X-plane, read its contiguous YZ block,
// split it into z-chunks, compress each chunk, and write to the sidecar file.
// Memory per plane: ny * nz * sizeof(float).

#ifdef ERWT3D_HAVE_LZ4

struct SidecarBuildState {
    int fdXp;
    int fdRaw;
    uint64_t nx, ny, nz;
    uint32_t stride;
    uint32_t chunkZRows;
    uint32_t chunksPerPlane;
    uint32_t planeCount;
    uint64_t totalChunks;
    uint64_t dataOffset;
    std::vector<XPChunkIndex> index;
    uint64_t chunkIdx;
    uint64_t totalStorageBytes;
    std::vector<char> compBuf;
    std::vector<float> rawRow;
    std::vector<std::vector<float>> planeChunks;
};


// Estimate compression ratio by sampling a few X planes.
static double estimateCompressionRatio(int fdRaw, uint64_t nx, uint64_t ny, uint64_t nz,
                                       uint32_t stride, uint32_t chunkZRows) {
    uint32_t chunksPerPlane = (nz + chunkZRows - 1) / chunkZRows;
    uint64_t chunkRawBytes = ny * static_cast<uint64_t>(chunkZRows) * sizeof(float);
    std::vector<char> compBuf(LZ4_compressBound(static_cast<int>(chunkRawBytes)));

    uint32_t sampleCount = 10;
    uint64_t planeCount = (nx + stride - 1) / stride;
    if (planeCount < sampleCount) sampleCount = planeCount;
    if (sampleCount == 0) return 1.0;

    std::vector<uint64_t> sampleXs;
    for (uint32_t s = 0; s < sampleCount; ++s) {
        uint64_t x = (static_cast<uint64_t>(s) * nx) / sampleCount;
        if (x >= nx) x = nx - 1;
        x = (x / stride) * stride;
        sampleXs.push_back(x);
    }

    uint64_t planeFloats = ny * nz;
    std::vector<char> rawPlane(planeFloats * sizeof(float));
    std::vector<float> planeChunk(ny * chunkZRows);

    uint64_t totalComp = 0, totalRaw = 0;
    for (uint32_t s = 0; s < sampleCount; ++s) {
        uint64_t x = sampleXs[s];
        uint64_t planeOff = x * planeFloats * sizeof(float);
        ssize_t rd = pread(fdRaw, rawPlane.data(), planeFloats * sizeof(float), planeOff);
        if (rd != static_cast<ssize_t>(planeFloats * sizeof(float))) return 1.0;
        const float* plane = reinterpret_cast<const float*>(rawPlane.data());

        for (uint32_t c = 0; c < chunksPerPlane; ++c) {
            uint64_t zStart = static_cast<uint64_t>(c) * chunkZRows;
            uint64_t zEnd = std::min(zStart + chunkZRows, nz);
            uint64_t rowsInChunk = zEnd - zStart;
            uint64_t thisRawBytes = rowsInChunk * ny * sizeof(float);

            for (uint64_t zi = 0; zi < rowsInChunk; ++zi) {
                uint64_t z = zStart + zi;
                float* dst = planeChunk.data() + zi * ny;
                for (uint64_t y = 0; y < ny; ++y)
                    dst[y] = plane[y * nz + z];
            }

            int cs = LZ4_compress_default(
                reinterpret_cast<const char*>(planeChunk.data()), compBuf.data(),
                static_cast<int>(thisRawBytes), static_cast<int>(compBuf.size()));
            if (cs > 0) { totalComp += cs; totalRaw += thisRawBytes; }
        }
    }
    if (totalRaw == 0) return 1.0;
    return static_cast<double>(totalComp) / static_cast<double>(totalRaw);
}

#endif

static int runSidecar(const std::string& rawPath, const std::string& erwtPath,
                      uint64_t nx, uint64_t ny, uint64_t nz,
                      uint32_t requestedStride, uint32_t chunkZRows,
                      double storageBudget = 1.45) {
#ifndef ERWT3D_HAVE_LZ4
    std::cerr << "Error: LZ4 support not compiled in. Sidecar mode requires LZ4." << std::endl;
    return 1;
#else
    std::string xpPath = erwtPath + ".xp";
    uint64_t rawBytes = nx * ny * nz * sizeof(float);
    uint64_t planeFloats = ny * nz;

    int fdRaw = open(rawPath.c_str(), O_RDONLY);
    if (fdRaw < 0) { perror("open raw"); return 1; }

    int fdErwt = open(erwtPath.c_str(), O_RDWR);
    if (fdErwt < 0) { perror("open erwt3d"); close(fdRaw); return 1; }

    ERWT3DHeader header;
    if (pread(fdErwt, &header, sizeof(header), 0) != sizeof(header)) {
        std::cerr << "Error reading ERWT3D header" << std::endl;
        close(fdRaw); close(fdErwt); return 1;
    }

    struct stat erwtStat;
    fstat(fdErwt, &erwtStat);
    uint64_t mainBytes = erwtStat.st_size;

    posix_fadvise(fdRaw, 0, 0, POSIX_FADV_SEQUENTIAL);

    // Estimate compression ratio
    std::cout << "Estimating sidecar compression ratio..." << std::endl;
    double ratio = estimateCompressionRatio(fdRaw, nx, ny, nz,
                                            std::max(requestedStride, 1u), chunkZRows);
    std::cout << "  Estimated compression ratio: " << ratio << "x" << std::endl;

    // Stride decision: start from stride=1, increase until storage fits budget
    uint32_t stride = 1;
    double projS = ratio * rawBytes;
    double totalRatio = static_cast<double>(mainBytes + projS) / rawBytes;
    std::cout << "  Projected total storage ratio (stride=1): " << totalRatio << "x" << std::endl;

    while (totalRatio > storageBudget) {
        stride++;
        projS = ratio * rawBytes / stride;
        totalRatio = static_cast<double>(mainBytes + projS) / rawBytes;
        std::cout << "  Trying stride=" << stride << ": " << totalRatio << "x" << std::endl;
        if (stride > nx) break;
    }

    if (totalRatio > storageBudget) {
        std::cout << "  Storage ratio too high (" << totalRatio
                  << "x > " << storageBudget << "x). Skipping sidecar." << std::endl;
        header.flags &= ~erwt3d::FLAG_HAS_XP_SIDECAR;
        header.reserved[21] = 0;
        pwrite(fdErwt, &header, sizeof(header), 0);
        close(fdRaw); close(fdErwt);
        return 0;
    }

    // Honor user-requested stride if it's larger
    if (requestedStride > stride) {
        stride = requestedStride;
        projS = ratio * rawBytes / stride;
        totalRatio = static_cast<double>(mainBytes + projS) / rawBytes;
        std::cout << "  Using requested stride=" << stride << ": " << totalRatio << "x" << std::endl;
    }

    uint32_t planeCount = (nx + stride - 1) / stride;
    uint32_t chunksPerPlane = (nz + chunkZRows - 1) / chunkZRows;
    uint64_t totalChunks = static_cast<uint64_t>(planeCount) * chunksPerPlane;
    uint64_t chunkRawBytes = ny * static_cast<uint64_t>(chunkZRows) * sizeof(float);

    std::cout << "Creating sidecar: " << xpPath << std::endl;
    std::cout << "  Stride: " << stride << ", Planes: " << planeCount
              << ", Chunks/plane: " << chunksPerPlane
              << ", Total chunks: " << totalChunks << std::endl;

    uint64_t memNeeded = static_cast<uint64_t>(planeCount) * chunkRawBytes;
    std::cout << "  Streaming memory per z-chunk batch: " << memNeeded / (1024*1024) << " MB" << std::endl;

    // Create sidecar file
    int fdXp = open(xpPath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fdXp < 0) { perror("open sidecar"); close(fdRaw); close(fdErwt); return 1; }

    // Write header placeholder
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
    if (pwrite(fdXp, &xpHdr, sizeof(xpHdr), 0) != sizeof(xpHdr)) {
        perror("write sidecar header"); close(fdXp); close(fdRaw); close(fdErwt); return 1;
    }

    // Initialize streaming build state
    SidecarBuildState s{};
    s.fdXp = fdXp;
    s.fdRaw = fdRaw;
    s.nx = nx; s.ny = ny; s.nz = nz;
    s.stride = stride;
    s.chunkZRows = chunkZRows;
    s.chunksPerPlane = chunksPerPlane;
    s.planeCount = planeCount;
    s.totalChunks = totalChunks;
    s.dataOffset = sizeof(xpHdr);
    s.index.resize(totalChunks);
    s.chunkIdx = 0;
    s.totalStorageBytes = 0;
    s.rawRow.resize(planeFloats);

    erwt3d::ThreadPool pool(std::min<uint32_t>(8, chunksPerPlane));

    std::cout << "Streaming build (plane-major, single read per plane)" << std::endl;
    for (uint32_t pi = 0; pi < planeCount; ++pi) {
        uint64_t x = static_cast<uint64_t>(pi) * stride;
        if (x >= nx) x = nx - 1;

        if (pi % 10 == 0 || pi + 1 == planeCount) {
            std::cout << "\r  plane " << (pi + 1) << "/" << planeCount
                      << " (" << ((pi + 1) * 100 / planeCount) << "%)" << std::flush;
        }

        uint64_t planeOff = x * planeFloats * sizeof(float);
        ssize_t rd = pread(fdRaw, s.rawRow.data(), planeFloats * sizeof(float), planeOff);
        if (rd != static_cast<ssize_t>(planeFloats * sizeof(float))) {
            std::cerr << "\nError reading raw X-plane at x=" << x << std::endl;
            close(fdXp); close(fdRaw); close(fdErwt); return 1;
        }
        const float* plane = s.rawRow.data();

        // Parallel compress all z-chunks for this plane
        using ChunkResult = std::vector<uint8_t>;
        std::vector<std::future<ChunkResult>> futures;
        for (uint32_t c = 0; c < chunksPerPlane; ++c) {
            futures.push_back(pool.submit([&, c, ny, plane]() -> ChunkResult {
                uint64_t zStart = static_cast<uint64_t>(c) * chunkZRows;
                uint64_t zEnd = std::min(zStart + chunkZRows, nz);
                uint64_t rowsInChunk = zEnd - zStart;

                std::vector<float> chunkBuf(ny * chunkZRows, 0.0f);
                float* dst = chunkBuf.data();
                for (uint64_t zi = 0; zi < rowsInChunk; ++zi) {
                    uint64_t z = zStart + zi;
                    for (uint64_t y = 0; y < ny; ++y)
                        dst[zi * ny + y] = plane[y * nz + z];
                }

                int bound = LZ4_compressBound(static_cast<int>(chunkRawBytes));
                ChunkResult out(bound);
                const char* src = reinterpret_cast<const char*>(chunkBuf.data());
                int cs = LZ4_compress_default(src, reinterpret_cast<char*>(out.data()),
                                              static_cast<int>(chunkRawBytes), bound);
                if (cs <= 0) return {};
                out.resize(static_cast<size_t>(cs));
                return out;
            }));
        }

        // Write results in chunk order
        for (uint32_t c = 0; c < chunksPerPlane; ++c) {
            ChunkResult compressed = futures[c].get();
            if (compressed.empty()) {
                std::cerr << "\nLZ4 compress failed for plane " << pi
                          << " chunk " << c << std::endl;
                close(fdXp); close(fdRaw); close(fdErwt); return 1;
            }

            uint64_t globalChunkIdx = static_cast<uint64_t>(pi) * chunksPerPlane + c;
            s.index[globalChunkIdx].chunk_offset = s.dataOffset + s.totalStorageBytes;
            s.index[globalChunkIdx].compressed_size = static_cast<uint32_t>(compressed.size());
            s.index[globalChunkIdx].raw_size = static_cast<uint32_t>(chunkRawBytes);
            s.totalStorageBytes += compressed.size();

            if (pwrite(fdXp, compressed.data(), compressed.size(),
                       s.index[globalChunkIdx].chunk_offset) != static_cast<ssize_t>(compressed.size())) {
                perror("write chunk");
                close(fdXp); close(fdRaw); close(fdErwt); return 1;
            }
        }
    }
    std::cout << "\r  plane " << planeCount << "/" << planeCount << " (100%)" << std::endl;

    close(fdRaw);

    // Write index at end
    uint64_t indexOffset = s.dataOffset + s.totalStorageBytes;
    ssize_t idxBytes = static_cast<ssize_t>(totalChunks * sizeof(XPChunkIndex));
    if (pwrite(fdXp, s.index.data(), idxBytes, indexOffset) != idxBytes) {
        perror("write index"); close(fdXp); close(fdErwt); return 1;
    }

    // Patch header
    xpHdr.index_offset = indexOffset;
    xpHdr.total_storage_bytes = s.totalStorageBytes;
    if (pwrite(fdXp, &xpHdr, sizeof(xpHdr), 0) != sizeof(xpHdr)) {
        perror("patch sidecar header"); close(fdXp); close(fdErwt); return 1;
    }

    // Update main ERWT3D header
    header.flags |= erwt3d::FLAG_HAS_XP_SIDECAR;
    header.reserved[21] = 1;
    if (pwrite(fdErwt, &header, sizeof(header), 0) != sizeof(header)) {
        std::cerr << "Warning: failed to update main header" << std::endl;
    }

    close(fdXp);
    close(fdErwt);

    double finalRatio = static_cast<double>(mainBytes + s.totalStorageBytes) / rawBytes;
    std::cout << "Done. Sidecar: " << s.totalStorageBytes / (1024 * 1024) << " MB compressed"
              << ", total storage ratio: " << finalRatio << "x" << std::endl;
    return 0;
#endif
}

static int runLegacy(const std::string& rawPath, const std::string& erwtPath,
                     uint64_t nx, uint64_t ny, uint64_t nz, uint32_t stride) {
    uint64_t planeFloats = ny * nz;
    uint64_t planeCount = (nx + stride - 1) / stride;
    uint64_t totalPlaneBytes = planeCount * planeFloats * sizeof(float);

    std::cout << "Pre-computing X planes from raw data (legacy mode)..." << std::endl;
    std::cout << "  Raw: " << rawPath << std::endl;
    std::cout << "  ERWT3D: " << erwtPath << std::endl;
    std::cout << "  Stride: " << stride << " (every " << stride << "th X value)" << std::endl;
    std::cout << "  Planes: " << planeCount << " x " << totalPlaneBytes / planeCount / (1024*1024) << " MB"
              << " = " << totalPlaneBytes / (1024*1024) << " MB" << std::endl;

    int fdRaw = open(rawPath.c_str(), O_RDONLY);
    if (fdRaw < 0) { perror("open raw"); return 1; }

    int fdErwt = open(erwtPath.c_str(), O_RDWR);
    if (fdErwt < 0) { perror("open erwt3d"); close(fdRaw); return 1; }

    struct stat st;
    fstat(fdErwt, &st);
    uint64_t xPlaneOffset = st.st_size;

    ERWT3DHeader header;
    pread(fdErwt, &header, sizeof(header), 0);

    posix_fadvise(fdRaw, 0, 0, POSIX_FADV_SEQUENTIAL);

    // Streaming: extract one X-plane at a time, write immediately.
    // A full X-plane is contiguous in the official Z-fastest raw layout.
    uint64_t planeBytes = planeFloats * sizeof(float);
    std::vector<char> rawPlane(planeBytes);
    std::vector<float> plane(planeFloats);

    std::cout << "Streaming extraction (one plane at a time)..." << std::endl;
    for (uint64_t pi = 0; pi < planeCount; ++pi) {
        uint64_t x = pi * stride;
        if (x >= nx) x = nx - 1;

        uint64_t planeOff = x * planeBytes;
        ssize_t rd = pread(fdRaw, rawPlane.data(), planeBytes, planeOff);
        if (rd != static_cast<ssize_t>(planeBytes)) {
            std::cerr << "\nError reading raw X-plane at x=" << x << std::endl;
            close(fdRaw); close(fdErwt);
            return 1;
        }
        const float* src = reinterpret_cast<const float*>(rawPlane.data());
        for (uint64_t z = 0; z < nz; ++z) {
            for (uint64_t y = 0; y < ny; ++y) {
                plane[z * ny + y] = src[y * nz + z];
            }
        }

        uint64_t off = xPlaneOffset + pi * planeBytes;
        ssize_t wr = pwrite(fdErwt, plane.data(), planeBytes, off);
        if (wr != static_cast<ssize_t>(planeBytes)) {
            std::cerr << "\nError writing plane " << pi << std::endl;
            close(fdRaw); close(fdErwt);
            return 1;
        }

        if (pi % 10 == 0) {
            std::cout << "\r  plane " << pi << "/" << planeCount
                      << " (" << (pi * 100 / planeCount) << "%)" << std::flush;
        }
    }
    std::cout << "\r  plane " << planeCount << "/" << planeCount << " (100%)" << std::endl;
    close(fdRaw);

    header.flags |= (1ULL << 3);
    header.reserved[16] = xPlaneOffset;
    header.reserved[17] = planeCount;
    header.reserved[18] = stride;
    if (pwrite(fdErwt, &header, sizeof(header), 0) != sizeof(header)) {
        std::cerr << "Error updating header" << std::endl;
    }

    close(fdErwt);
    std::cout << "Done. X-plane offset: " << xPlaneOffset
              << " (" << totalPlaneBytes / (1024*1024) << " MB stored)" << std::endl;
    return 0;
}

int main(int argc, char* argv[]) {
    std::string rawPath, erwtPath;
    uint64_t nx = 0, ny = 0, nz = 0;
    uint32_t stride = 0;
    std::string mode = "sidecar";
    uint32_t chunkZRows = 256;
    double storageBudget = 1.45;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--raw" && i + 1 < argc) rawPath = argv[++i];
        else if (arg == "--erwt3d" && i + 1 < argc) erwtPath = argv[++i];
        else if (arg == "--nx" && i + 1 < argc) nx = std::stoull(argv[++i]);
        else if (arg == "--ny" && i + 1 < argc) ny = std::stoull(argv[++i]);
        else if (arg == "--nz" && i + 1 < argc) nz = std::stoull(argv[++i]);
        else if (arg == "--stride" && i + 1 < argc) stride = std::stoul(argv[++i]);
        else if (arg == "--mode" && i + 1 < argc) mode = argv[++i];
        else if (arg == "--chunk-z-rows" && i + 1 < argc) chunkZRows = std::stoul(argv[++i]);
        else if (arg == "--storage-budget" && i + 1 < argc) storageBudget = std::stod(argv[++i]);
    }

    if (rawPath.empty() || erwtPath.empty() || nx == 0 || ny == 0 || nz == 0) {
        std::cerr << "Usage: erwt3d_precompute_x --raw data.raw --erwt3d data.erwt3d"
                  << " --nx N --ny N --nz N [options]" << std::endl;
        std::cerr << "  --mode sidecar|legacy (default: sidecar)" << std::endl;
        std::cerr << "  --stride N (sidecar: auto from 1, legacy: default 1)" << std::endl;
        std::cerr << "  --chunk-z-rows N (sidecar, default: 256)" << std::endl;
        std::cerr << "  --storage-budget X (sidecar, default: 1.45)" << std::endl;
        return 1;
    }

    if (mode == "sidecar") {
        uint32_t hintStride = (stride == 0) ? 1 : stride;
        return runSidecar(rawPath, erwtPath, nx, ny, nz, hintStride, chunkZRows, storageBudget);
    } else {
        if (stride == 0) stride = 1;
        if (stride < 1) stride = 1;
        return runLegacy(rawPath, erwtPath, nx, ny, nz, stride);
    }
}
