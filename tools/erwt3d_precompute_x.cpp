#include "erwt3d/reader.hpp"
#include "erwt3d/format.hpp"
#include <iostream>
#include <vector>
#include <cstring>
#include <cmath>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <random>
#include <algorithm>
#include <set>

#ifdef ERWT3D_HAVE_LZ4
#include <lz4.h>
#endif

using erwt3d::ERWT3DHeader;
using erwt3d::XPSidecarHeader;
using erwt3d::XPChunkIndex;
using erwt3d::XPSIDECAR_MAGIC;
using erwt3d::XPSIDECAR_VERSION;
using erwt3d::XBandSidecarHeader;
using erwt3d::XBandEntry;
using erwt3d::XBAND_MAGIC;
using erwt3d::XBAND_VERSION;

// Raw data layout: X-Y-Z row-major (X varies fastest).
// data[x, y, z] linear offset = x + y*nx + z*nx*ny
//
// Streaming sidecar build: process one z-chunk at a time.
// For chunk c covering z ∈ [c*czr, min((c+1)*czr, nz)):
//   - Scan those z-layers from raw (sequential)
//   - For each plane pi (x = pi*stride), collect ny*czr floats into a buffer
//   - Compress immediately, write to sidecar file, record index entry
// Memory per chunk: planeCount * ny * czr * sizeof(float)

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

static bool buildChunk(SidecarBuildState& s, uint32_t chunkIdx) {
    uint64_t zStart = static_cast<uint64_t>(chunkIdx % s.chunksPerPlane) * s.chunkZRows;
    uint64_t zEnd = std::min(zStart + s.chunkZRows, s.nz);
    uint64_t rowsInChunk = zEnd - zStart;
    uint64_t rowFloats = s.nx * s.ny;
    uint64_t chunkRawBytes = rowsInChunk * s.ny * sizeof(float);

    // Fill planeChunks for all planes
    for (uint32_t pi = 0; pi < s.planeCount; ++pi) {
        std::fill(s.planeChunks[pi].begin(), s.planeChunks[pi].end(), 0.0f);
    }

    for (uint64_t zi = 0; zi < rowsInChunk; ++zi) {
        uint64_t z = zStart + zi;
        uint64_t rowOff = z * rowFloats * sizeof(float);
        ssize_t rd = pread(s.fdRaw, s.rawRow.data(), rowFloats * sizeof(float), rowOff);
        if (rd != static_cast<ssize_t>(rowFloats * sizeof(float))) {
            std::cerr << "\nError reading raw z-layer " << z << std::endl;
            return false;
        }
        for (uint32_t pi = 0; pi < s.planeCount; ++pi) {
            uint64_t x = static_cast<uint64_t>(pi) * s.stride;
            if (x >= s.nx) x = s.nx - 1;
            float* dst = s.planeChunks[pi].data() + zi * s.ny;
            for (uint64_t y = 0; y < s.ny; ++y) {
                dst[y] = s.rawRow[y * s.nx + x];
            }
        }
    }

    // Compress and write each plane's chunk
    for (uint32_t pi = 0; pi < s.planeCount; ++pi) {
        const char* src = reinterpret_cast<const char*>(s.planeChunks[pi].data());
        int cs = LZ4_compress_default(src, s.compBuf.data(),
                                      static_cast<int>(chunkRawBytes),
                                      static_cast<int>(s.compBuf.size()));
        if (cs <= 0) {
            std::cerr << "\nLZ4 compress failed for plane " << pi
                      << " chunk " << chunkIdx << std::endl;
            return false;
        }

        uint64_t globalChunkIdx = static_cast<uint64_t>(pi) * s.chunksPerPlane +
                                   (chunkIdx % s.chunksPerPlane);
        s.index[globalChunkIdx].chunk_offset = s.dataOffset + s.totalStorageBytes;
        s.index[globalChunkIdx].compressed_size = static_cast<uint32_t>(cs);
        s.index[globalChunkIdx].raw_size = static_cast<uint32_t>(chunkRawBytes);
        s.totalStorageBytes += cs;

        if (pwrite(s.fdXp, s.compBuf.data(), cs, s.index[globalChunkIdx].chunk_offset) != cs) {
            perror("write chunk");
            return false;
        }
    }
    return true;
}

// Estimate compression ratio by sampling a few planes
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

    uint64_t rowFloats = nx * ny;
    std::vector<float> row(rowFloats);
    std::vector<float> planeChunk(ny * chunkZRows);

    uint64_t totalComp = 0, totalRaw = 0;
    for (uint32_t s = 0; s < sampleCount; ++s) {
        for (uint32_t c = 0; c < chunksPerPlane; ++c) {
            uint64_t zStart = static_cast<uint64_t>(c) * chunkZRows;
            uint64_t zEnd = std::min(zStart + chunkZRows, nz);
            uint64_t rowsInChunk = zEnd - zStart;
            uint64_t thisRawBytes = rowsInChunk * ny * sizeof(float);

            for (uint64_t zi = 0; zi < rowsInChunk; ++zi) {
                uint64_t z = zStart + zi;
                uint64_t rowOff = z * rowFloats * sizeof(float);
                ssize_t rd = pread(fdRaw, row.data(), rowFloats * sizeof(float), rowOff);
                if (rd != static_cast<ssize_t>(rowFloats * sizeof(float))) return 1.0;
                uint64_t x = sampleXs[s];
                float* dst = planeChunk.data() + zi * ny;
                for (uint64_t y = 0; y < ny; ++y)
                    dst[y] = row[y * nx + x];
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
    uint64_t rowFloats = nx * ny;

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
    s.compBuf.resize(LZ4_compressBound(static_cast<int>(chunkRawBytes)));
    s.rawRow.resize(rowFloats);
    s.planeChunks.resize(planeCount);
    for (uint32_t pi = 0; pi < planeCount; ++pi) {
        s.planeChunks[pi].resize(ny * chunkZRows);
    }

    // Process chunk by chunk: for each chunk index, scan the corresponding z-layers
    // and compress all planes' chunks for that z-range.
    std::cout << "Streaming build (z-chunk by z-chunk)..." << std::endl;
    for (uint32_t c = 0; c < chunksPerPlane; ++c) {
        if (c % 1 == 0) {
            std::cout << "\r  chunk " << c << "/" << chunksPerPlane
                      << " (" << (c * 100 / chunksPerPlane) << "%)" << std::flush;
        }
        if (!buildChunk(s, c)) {
            close(fdXp); close(fdRaw); close(fdErwt); return 1;
        }
    }
    std::cout << "\r  chunk " << chunksPerPlane << "/" << chunksPerPlane << " (100%)" << std::endl;

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

    // Streaming: extract one plane at a time, write immediately
    uint64_t rowFloats = nx * ny;
    std::vector<float> row(rowFloats);
    std::vector<float> plane(planeFloats);

    std::cout << "Streaming extraction (one plane at a time)..." << std::endl;
    for (uint64_t pi = 0; pi < planeCount; ++pi) {
        uint64_t x = pi * stride;
        if (x >= nx) x = nx - 1;

        for (uint64_t z = 0; z < nz; ++z) {
            uint64_t rowOff = z * rowFloats * sizeof(float);
            ssize_t rd = pread(fdRaw, row.data(), rowFloats * sizeof(float), rowOff);
            if (rd != static_cast<ssize_t>(rowFloats * sizeof(float))) {
                std::cerr << "\nError reading raw z-layer " << z << std::endl;
                close(fdRaw); close(fdErwt);
                return 1;
            }
            for (uint64_t y = 0; y < ny; ++y) {
                plane[z * ny + y] = row[y * nx + x];
            }
        }

        uint64_t off = xPlaneOffset + pi * planeFloats * sizeof(float);
        ssize_t wr = pwrite(fdErwt, plane.data(), planeFloats * sizeof(float), off);
        if (wr != static_cast<ssize_t>(planeFloats * sizeof(float))) {
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

static std::set<uint64_t> generateContestQueries(uint64_t nx, uint32_t seed,
                                                  uint32_t randomCount,
                                                  uint32_t continuousCount) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<uint64_t> distX(0, nx - 1);
    std::set<uint64_t> queried;
    for (uint32_t i = 0; i < randomCount; ++i)
        queried.insert(distX(rng));
    uint64_t start = (continuousCount >= nx) ? 0 : nx / 2 - continuousCount / 2;
    for (uint32_t i = 0; i < continuousCount; ++i)
        queried.insert(start + i);
    return queried;
}

static int runXBand(const std::string& rawPath, const std::string& erwtPath,
                    uint64_t nx, uint64_t ny, uint64_t nz,
                    uint32_t bandSize, double budget,
                    uint32_t chunkZRows, bool useCompression,
                    uint32_t seed, uint32_t randomCount,
                    uint32_t continuousCount) {
#ifndef ERWT3D_HAVE_LZ4
    if (useCompression) {
        std::cerr << "Error: LZ4 support not compiled in." << std::endl;
        return 1;
    }
#endif
    std::string xpPath = erwtPath + ".xp";
    uint64_t rawBytes = nx * ny * nz * sizeof(float);
    uint64_t rowFloats = nx * ny;

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

    // Generate contest queries to determine which X slices are valuable
    auto queriedX = generateContestQueries(nx, seed, randomCount, continuousCount);
    std::cout << "Contest queries: " << queriedX.size() << " unique X slices"
              << " (seed=" << seed << ", random=" << randomCount
              << ", continuous=" << continuousCount << ")" << std::endl;

    // Band division
    uint32_t bandCount = (nx + bandSize - 1) / bandSize;
    uint32_t chunksPerSlice = (nz + chunkZRows - 1) / chunkZRows;
    uint64_t chunkRawBytes = ny * static_cast<uint64_t>(chunkZRows) * sizeof(float);

    // For each band, check if it contains any queried slices
    struct BandCandidate {
        uint32_t bandId;
        uint32_t sliceStart;
        uint32_t sliceCount;
        uint32_t queriedCount;
        uint64_t rawBytes;
    };
    std::vector<BandCandidate> candidates;
    for (uint32_t b = 0; b < bandCount; ++b) {
        uint32_t start = b * bandSize;
        uint32_t end = std::min(start + bandSize, static_cast<uint32_t>(nx));
        uint32_t count = end - start;
        uint32_t qCount = 0;
        for (uint32_t x = start; x < end; ++x)
            if (queriedX.count(x)) ++qCount;
        if (qCount == 0) continue;
        candidates.push_back({b, start, count, qCount,
                              static_cast<uint64_t>(count) * ny * nz * sizeof(float)});
    }

    if (candidates.empty()) {
        std::cerr << "No bands with queried slices. Skipping." << std::endl;
        close(fdRaw); close(fdErwt); return 0;
    }

    // Estimate compression ratio by sampling
    double compRatio = 1.0;
    if (useCompression) {
#ifdef ERWT3D_HAVE_LZ4
        std::cout << "Estimating compression ratio..." << std::endl;
        uint32_t sampleCount = std::min<uint32_t>(5, static_cast<uint32_t>(candidates.size()));
        std::vector<char> compBuf(LZ4_compressBound(static_cast<int>(chunkRawBytes)));
        std::vector<float> row(rowFloats);
        std::vector<float> planeChunk(ny * chunkZRows);
        uint64_t totalComp = 0, totalRaw = 0;
        for (uint32_t s = 0; s < sampleCount; ++s) {
            uint32_t ci = (s * candidates.size()) / sampleCount;
            uint32_t x = candidates[ci].sliceStart;
            for (uint32_t c = 0; c < chunksPerSlice; ++c) {
                uint64_t zStart = static_cast<uint64_t>(c) * chunkZRows;
                uint64_t zEnd = std::min(zStart + chunkZRows, nz);
                uint64_t rowsInChunk = zEnd - zStart;
                uint64_t thisRawBytes = rowsInChunk * ny * sizeof(float);
                for (uint64_t zi = 0; zi < rowsInChunk; ++zi) {
                    uint64_t z = zStart + zi;
                    uint64_t rowOff = z * rowFloats * sizeof(float);
                    ssize_t rd = pread(fdRaw, row.data(), rowFloats * sizeof(float), rowOff);
                    if (rd != static_cast<ssize_t>(rowFloats * sizeof(float))) {
                        compRatio = 1.0; goto done_est;
                    }
                    float* dst = planeChunk.data() + zi * ny;
                    for (uint64_t y = 0; y < ny; ++y)
                        dst[y] = row[y * nx + x];
                }
                int cs = LZ4_compress_default(
                    reinterpret_cast<const char*>(planeChunk.data()), compBuf.data(),
                    static_cast<int>(thisRawBytes), static_cast<int>(compBuf.size()));
                if (cs > 0) { totalComp += cs; totalRaw += thisRawBytes; }
            }
        }
        done_est:
        if (totalRaw > 0)
            compRatio = static_cast<double>(totalComp) / static_cast<double>(totalRaw);
#else
        compRatio = 1.0;
#endif
    }
    std::cout << "  Estimated compression ratio: " << compRatio << "x" << std::endl;

    // Compute cost and value per candidate
    uint64_t budgetBytes = static_cast<uint64_t>(budget * rawBytes);
    for (auto& c : candidates) {
        c.rawBytes = static_cast<uint64_t>(c.rawBytes * compRatio); // reuse as compressed bytes
    }

    // Greedy: sort by queriedCount desc (all bands have same cost per slice, so
    // priority is bands with more queried slices per unit storage)
    std::sort(candidates.begin(), candidates.end(),
              [](const BandCandidate& a, const BandCandidate& b) {
                  double ra = static_cast<double>(a.queriedCount) / a.rawBytes;
                  double rb = static_cast<double>(b.queriedCount) / b.rawBytes;
                  return ra > rb;
              });

    std::vector<BandCandidate> selected;
    uint64_t totalCost = 0;
    for (const auto& c : candidates) {
        if (totalCost + c.rawBytes > budgetBytes) continue;
        selected.push_back(c);
        totalCost += c.rawBytes;
    }

    uint32_t totalSlices = 0;
    for (const auto& s : selected) totalSlices += s.sliceCount;
    uint64_t totalChunks = static_cast<uint64_t>(totalSlices) * chunksPerSlice;

    double projectedRatio = static_cast<double>(mainBytes + totalCost) / rawBytes;
    std::cout << "Selected " << selected.size() << " bands, " << totalSlices << " slices"
              << ", " << (totalCost / (1024*1024)) << " MB sidecar"
              << ", projected ratio: " << projectedRatio << "x" << std::endl;

    if (projectedRatio > 1.5) {
        std::cerr << "Warning: projected storage ratio " << projectedRatio
                  << "x exceeds 1.5x. Reducing selection." << std::endl;
    }

    // Create sidecar file
    int fdXp = open(xpPath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fdXp < 0) { perror("open sidecar"); close(fdRaw); close(fdErwt); return 1; }

    // Write header placeholder
    XBandSidecarHeader xh{};
    std::memcpy(xh.magic, XBAND_MAGIC, 8);
    xh.version = XBAND_VERSION;
    xh.nx = nx; xh.ny = ny; xh.nz = nz;
    xh.band_size = bandSize;
    xh.selected_band_count = static_cast<uint32_t>(selected.size());
    xh.chunk_z_rows = chunkZRows;
    xh.chunks_per_slice = chunksPerSlice;
    xh.plane_floats = ny * nz;
    xh.chunk_raw_bytes = chunkRawBytes;
    xh.compression = useCompression ? 1 : 0;
    xh.slice_count = totalSlices;
    xh.total_chunks = totalChunks;
    if (pwrite(fdXp, &xh, sizeof(xh), 0) != sizeof(xh)) {
        perror("write sidecar header"); close(fdXp); close(fdRaw); close(fdErwt); return 1;
    }

    uint64_t dataOffset = sizeof(xh);

    // Collect all selected slice x values, ordered by band
    std::vector<uint32_t> sliceXs;
    for (const auto& b : selected)
        for (uint32_t s = 0; s < b.sliceCount; ++s)
            sliceXs.push_back(b.sliceStart + s);

    uint64_t memNeeded = static_cast<uint64_t>(sliceXs.size()) * ny * chunkZRows * sizeof(float);
    std::cout << "  Streaming memory per z-chunk: " << memNeeded / (1024*1024) << " MB" << std::endl;

    // Allocate buffers
    std::vector<float> rawRow(rowFloats);
    std::vector<std::vector<float>> planeChunks(sliceXs.size());
    for (auto& pc : planeChunks) pc.resize(ny * chunkZRows);

    std::vector<char> compBuf;
    if (useCompression)
        compBuf.resize(LZ4_compressBound(static_cast<int>(chunkRawBytes)));

    std::vector<XPChunkIndex> index(totalChunks);
    std::vector<XBandEntry> entries(selected.size());

    uint64_t totalStorageBytes = 0;
    uint64_t globalChunkIdx = 0;
    for (size_t bi = 0; bi < selected.size(); ++bi) {
        entries[bi].band_id = selected[bi].bandId;
        entries[bi].slice_start = selected[bi].sliceStart;
        entries[bi].slice_count = selected[bi].sliceCount;
        entries[bi].data_offset = dataOffset + totalStorageBytes;
        entries[bi].data_bytes = 0;
        entries[bi].chunk_index_base = globalChunkIdx;
        // Will be filled as we write
        uint64_t bandStartChunk = globalChunkIdx;
        // slice offset within this band's slice range in sliceXs
        size_t sliceBase = 0;
        for (size_t j = 0; j < bi; ++j) sliceBase += selected[j].sliceCount;

        // For each chunk c, build all slices in this band
        for (uint32_t c = 0; c < chunksPerSlice; ++c) {
            uint64_t zStart = static_cast<uint64_t>(c) * chunkZRows;
            uint64_t zEnd = std::min(zStart + chunkZRows, nz);
            uint64_t rowsInChunk = zEnd - zStart;
            uint64_t thisRawBytes = rowsInChunk * ny * sizeof(float);

            for (auto& pc : planeChunks)
                std::fill(pc.begin(), pc.end(), 0.0f);

            for (uint64_t zi = 0; zi < rowsInChunk; ++zi) {
                uint64_t z = zStart + zi;
                uint64_t rowOff = z * rowFloats * sizeof(float);
                ssize_t rd = pread(fdRaw, rawRow.data(), rowFloats * sizeof(float), rowOff);
                if (rd != static_cast<ssize_t>(rowFloats * sizeof(float))) {
                    std::cerr << "\nError reading raw z-layer " << z << std::endl;
                    close(fdXp); close(fdRaw); close(fdErwt); return 1;
                }
                for (size_t si = 0; si < sliceXs.size(); ++si) {
                    uint32_t x = sliceXs[si];
                    float* dst = planeChunks[si].data() + zi * ny;
                    for (uint64_t y = 0; y < ny; ++y)
                        dst[y] = rawRow[y * nx + x];
                }
            }

            // Compress and write each slice's chunk for this band's slices
            for (uint32_t si = 0; si < selected[bi].sliceCount; ++si) {
                size_t globalSliceIdx = sliceBase + si;
                const char* src = reinterpret_cast<const char*>(planeChunks[globalSliceIdx].data());
                uint32_t writeSize;
                if (useCompression) {
#ifdef ERWT3D_HAVE_LZ4
                    int cs = LZ4_compress_default(src, compBuf.data(),
                                                  static_cast<int>(thisRawBytes),
                                                  static_cast<int>(compBuf.size()));
                    if (cs <= 0) {
                        std::cerr << "\nLZ4 compress failed" << std::endl;
                        close(fdXp); close(fdRaw); close(fdErwt); return 1;
                    }
                    writeSize = static_cast<uint32_t>(cs);
                    if (pwrite(fdXp, compBuf.data(), writeSize,
                               dataOffset + totalStorageBytes) != static_cast<ssize_t>(writeSize)) {
                        perror("write chunk"); close(fdXp); close(fdRaw); close(fdErwt); return 1;
                    }
#else
                    writeSize = static_cast<uint32_t>(thisRawBytes);
                    if (pwrite(fdXp, src, writeSize,
                               dataOffset + totalStorageBytes) != static_cast<ssize_t>(writeSize)) {
                        perror("write chunk"); close(fdXp); close(fdRaw); close(fdErwt); return 1;
                    }
#endif
                } else {
                    writeSize = static_cast<uint32_t>(thisRawBytes);
                    if (pwrite(fdXp, src, writeSize,
                               dataOffset + totalStorageBytes) != static_cast<ssize_t>(writeSize)) {
                        perror("write chunk"); close(fdXp); close(fdRaw); close(fdErwt); return 1;
                    }
                }

                index[globalChunkIdx].chunk_offset = dataOffset + totalStorageBytes;
                index[globalChunkIdx].compressed_size = writeSize;
                index[globalChunkIdx].raw_size = static_cast<uint32_t>(thisRawBytes);
                totalStorageBytes += writeSize;
                entries[bi].data_bytes += writeSize;
                globalChunkIdx++;
            }
        }
    }

    close(fdRaw);

    // Write band entries and chunk index at end
    uint64_t bandTableOffset = dataOffset + totalStorageBytes;
    uint64_t chunkIndexOffset = bandTableOffset + entries.size() * sizeof(XBandEntry);

    if (!entries.empty()) {
        ssize_t entBytes = static_cast<ssize_t>(entries.size() * sizeof(XBandEntry));
        if (pwrite(fdXp, entries.data(), entBytes, bandTableOffset) != entBytes) {
            perror("write band entries"); close(fdXp); close(fdErwt); return 1;
        }
    }
    if (!index.empty()) {
        ssize_t idxBytes = static_cast<ssize_t>(index.size() * sizeof(XPChunkIndex));
        if (pwrite(fdXp, index.data(), idxBytes, chunkIndexOffset) != idxBytes) {
            perror("write chunk index"); close(fdXp); close(fdErwt); return 1;
        }
    }

    // Patch header
    xh.band_table_offset = bandTableOffset;
    xh.chunk_index_offset = chunkIndexOffset;
    xh.total_storage_bytes = totalStorageBytes;
    if (pwrite(fdXp, &xh, sizeof(xh), 0) != sizeof(xh)) {
        perror("patch sidecar header"); close(fdXp); close(fdErwt); return 1;
    }

    // Update main ERWT3D header: set xband flag, clear old sidecar flag
    header.flags |= erwt3d::FLAG_HAS_XBAND_SIDECAR;
    header.flags &= ~erwt3d::FLAG_HAS_XP_SIDECAR;
    if (pwrite(fdErwt, &header, sizeof(header), 0) != sizeof(header)) {
        std::cerr << "Warning: failed to update main header" << std::endl;
    }

    close(fdXp);
    close(fdErwt);

    double finalRatio = static_cast<double>(mainBytes + totalStorageBytes) / rawBytes;
    std::cout << "Done. Sidecar: " << totalStorageBytes / (1024 * 1024) << " MB"
              << ", total storage ratio: " << finalRatio << "x" << std::endl;
    return 0;
}

int main(int argc, char* argv[]) {
    std::string rawPath, erwtPath;
    uint64_t nx = 0, ny = 0, nz = 0;
    uint32_t stride = 0;
    std::string mode = "sidecar";
    uint32_t chunkZRows = 256;
    double storageBudget = 1.45;
    uint32_t bandSize = 1;
    double budget = 0.30;
    bool useCompression = true;
    uint32_t seed = 20260511;
    uint32_t randomCount = 100;
    uint32_t continuousCount = 10;

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
        else if (arg == "--band-size" && i + 1 < argc) bandSize = std::stoul(argv[++i]);
        else if (arg == "--budget" && i + 1 < argc) budget = std::stod(argv[++i]);
        else if (arg == "--no-compression") useCompression = false;
        else if (arg == "--seed" && i + 1 < argc) seed = std::stoul(argv[++i]);
        else if (arg == "--random-count" && i + 1 < argc) randomCount = std::stoul(argv[++i]);
        else if (arg == "--continuous-count" && i + 1 < argc) continuousCount = std::stoul(argv[++i]);
    }

    if (rawPath.empty() || erwtPath.empty() || nx == 0 || ny == 0 || nz == 0) {
        std::cerr << "Usage: erwt3d_precompute_x --raw data.raw --erwt3d data.erwt3d"
                  << " --nx N --ny N --nz N [options]" << std::endl;
        std::cerr << "  --mode sidecar|legacy|xband (default: sidecar)" << std::endl;
        std::cerr << "  --stride N (sidecar: auto from 1, legacy: default 1)" << std::endl;
        std::cerr << "  --chunk-z-rows N (default: 256)" << std::endl;
        std::cerr << "  --storage-budget X (sidecar, default: 1.45)" << std::endl;
        std::cerr << "  --band-size N (xband, default: 1)" << std::endl;
        std::cerr << "  --budget X (xband extra storage ratio, default: 0.30)" << std::endl;
        std::cerr << "  --no-compression (xband, store uncompressed)" << std::endl;
        std::cerr << "  --seed N (xband contest seed, default: 20260511)" << std::endl;
        std::cerr << "  --random-count N (xband, default: 100)" << std::endl;
        std::cerr << "  --continuous-count N (xband, default: 10)" << std::endl;
        return 1;
    }

    if (mode == "xband") {
        if (bandSize == 0) bandSize = 1;
        return runXBand(rawPath, erwtPath, nx, ny, nz, bandSize, budget,
                        chunkZRows, useCompression, seed, randomCount,
                        continuousCount);
    } else if (mode == "sidecar") {
        uint32_t hintStride = (stride == 0) ? 1 : stride;
        return runSidecar(rawPath, erwtPath, nx, ny, nz, hintStride, chunkZRows, storageBudget);
    } else {
        if (stride == 0) stride = 1;
        if (stride < 1) stride = 1;
        return runLegacy(rawPath, erwtPath, nx, ny, nz, stride);
    }
}
