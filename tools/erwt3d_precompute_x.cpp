#include "erwt3d/reader.hpp"
#include "erwt3d/format.hpp"
#include <iostream>
#include <vector>
#include <cstring>
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

// Raw data layout: X-Y-Z row-major (X varies fastest).
// data[x, y, z] linear offset = x + y*nx + z*nx*ny
// A fixed-x YZ plane is strided in raw: plane[y][z] at offset x + y*nx + z*nx*ny
//
// Extraction strategy: sequential scan of raw file by z-layer.
// For each z, read the full nx*ny XY plane, then scatter the x-th column
// into every sidecar plane's z-row. This is one sequential pass over raw.

#ifdef ERWT3D_HAVE_LZ4
static double compressPlanesToSidecar(
    int fdXp, const std::vector<std::vector<float>>& planes,
    uint64_t ny, uint64_t nz, uint32_t stride, uint32_t chunkZRows,
    std::vector<XPChunkIndex>& index, uint64_t dataOffset) {

    uint64_t planeCount = planes.size();
    uint32_t chunksPerPlane = (nz + chunkZRows - 1) / chunkZRows;
    uint64_t chunkRawBytes = ny * static_cast<uint64_t>(chunkZRows) * sizeof(float);
    std::vector<char> compBuf(LZ4_compressBound(static_cast<int>(chunkRawBytes)));

    uint64_t chunkIdx = 0;
    uint64_t totalStorageBytes = 0;

    for (uint64_t pi = 0; pi < planeCount; ++pi) {
        const float* planeData = planes[pi].data();
        for (uint32_t c = 0; c < chunksPerPlane; ++c) {
            uint64_t zStart = static_cast<uint64_t>(c) * chunkZRows;
            uint64_t zEnd = std::min(zStart + chunkZRows, nz);
            uint64_t thisRawBytes = (zEnd - zStart) * ny * sizeof(float);
            const char* src = reinterpret_cast<const char*>(planeData + zStart * ny);

            int cs = LZ4_compress_default(src, compBuf.data(),
                                          static_cast<int>(thisRawBytes),
                                          static_cast<int>(compBuf.size()));
            if (cs <= 0) {
                std::cerr << "\nLZ4 compress failed for plane " << pi
                          << " chunk " << c << std::endl;
                return -1.0;
            }

            index[chunkIdx].chunk_offset = dataOffset + totalStorageBytes;
            index[chunkIdx].compressed_size = static_cast<uint32_t>(cs);
            index[chunkIdx].raw_size = static_cast<uint32_t>(thisRawBytes);
            totalStorageBytes += cs;

            if (pwrite(fdXp, compBuf.data(), cs, index[chunkIdx].chunk_offset) != cs) {
                perror("write chunk");
                return -1.0;
            }
            ++chunkIdx;
        }
    }
    return static_cast<double>(totalStorageBytes);
}
#endif

static int runSidecar(const std::string& rawPath, const std::string& erwtPath,
                      uint64_t nx, uint64_t ny, uint64_t nz,
                      uint32_t requestedStride, uint32_t chunkZRows) {
#ifndef ERWT3D_HAVE_LZ4
    std::cerr << "Error: LZ4 support not compiled in. Sidecar mode requires LZ4." << std::endl;
    return 1;
#else
    std::string xpPath = erwtPath + ".xp";

    uint64_t planeFloats = ny * nz;
    uint64_t rawBytes = nx * ny * nz * sizeof(float);

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

    // Stride decision: try stride=1 first (100% hit), increase if storage exceeds 1.45x
    // We estimate compression by sampling a few planes via sequential scan.
    std::cout << "Estimating sidecar compression ratio..." << std::endl;

    uint32_t sampleStride = std::max(requestedStride, 1u);
    uint32_t sampleCount = 10;
    uint64_t samplePlaneCount = (nx + sampleStride - 1) / sampleStride;
    if (samplePlaneCount < sampleCount) sampleCount = samplePlaneCount;
    if (sampleCount == 0) sampleCount = 1;

    uint32_t chunksPerPlane = (nz + chunkZRows - 1) / chunkZRows;
    uint64_t chunkRawBytes = ny * static_cast<uint64_t>(chunkZRows) * sizeof(float);
    std::vector<char> compBuf(LZ4_compressBound(static_cast<int>(chunkRawBytes)));

    // Sample planes: sequential scan, extract every sampleStride-th x
    std::vector<std::vector<float>> samplePlanes(sampleCount, std::vector<float>(planeFloats));
    uint64_t rowFloats = nx * ny;
    std::vector<float> row(rowFloats);

    // Determine which x values to sample
    std::vector<uint64_t> sampleXs;
    for (uint32_t s = 0; s < sampleCount; ++s) {
        uint64_t x = (static_cast<uint64_t>(s) * nx) / sampleCount;
        if (x >= nx) x = nx - 1;
        // Align to sampleStride
        x = (x / sampleStride) * sampleStride;
        sampleXs.push_back(x);
    }

    std::cout << "  Scanning raw to extract " << sampleCount << " sample planes..." << std::endl;
    for (uint64_t z = 0; z < nz; ++z) {
        uint64_t rowOff = z * rowFloats * sizeof(float);
        ssize_t rd = pread(fdRaw, row.data(), rowFloats * sizeof(float), rowOff);
        if (rd != static_cast<ssize_t>(rowFloats * sizeof(float))) {
            std::cerr << "\nError reading raw z-layer " << z << std::endl;
            close(fdRaw); close(fdErwt); return 1;
        }
        for (uint32_t s = 0; s < sampleCount; ++s) {
            uint64_t x = sampleXs[s];
            for (uint64_t y = 0; y < ny; ++y) {
                samplePlanes[s][z * ny + y] = row[y * nx + x];
            }
        }
        if (z % 100 == 0) {
            std::cout << "\r  z=" << z << "/" << nz << " (" << (z * 100 / nz) << "%)" << std::flush;
        }
    }
    std::cout << "\r  z=" << nz << "/" << nz << " (100%)" << std::endl;

    // Compress samples to estimate ratio
    uint64_t totalComp = 0, totalRaw = 0;
    for (uint32_t s = 0; s < sampleCount; ++s) {
        const float* planeData = samplePlanes[s].data();
        for (uint32_t c = 0; c < chunksPerPlane; ++c) {
            uint64_t zStart = static_cast<uint64_t>(c) * chunkZRows;
            uint64_t zEnd = std::min(zStart + chunkZRows, nz);
            uint64_t thisRawBytes = (zEnd - zStart) * ny * sizeof(float);
            const char* src = reinterpret_cast<const char*>(planeData + zStart * ny);
            int cs = LZ4_compress_default(src, compBuf.data(),
                                          static_cast<int>(thisRawBytes),
                                          static_cast<int>(compBuf.size()));
            if (cs > 0) {
                totalComp += cs;
                totalRaw += thisRawBytes;
            }
        }
    }
    double ratio = (totalRaw > 0) ? static_cast<double>(totalComp) / static_cast<double>(totalRaw) : 1.0;
    std::cout << "  Estimated compression ratio: " << ratio << "x" << std::endl;

    // Stride decision: start from stride=1, increase until storage fits
    uint32_t stride = 1;
    double projS = ratio * rawBytes;
    double totalRatio = static_cast<double>(mainBytes + projS) / rawBytes;
    std::cout << "  Projected total storage ratio (stride=1): " << totalRatio << "x" << std::endl;

    while (totalRatio > 1.45 && stride < 8) {
        stride++;
        projS = ratio * rawBytes / stride;
        totalRatio = static_cast<double>(mainBytes + projS) / rawBytes;
        std::cout << "  Trying stride=" << stride << ": " << totalRatio << "x" << std::endl;
    }

    if (totalRatio > 1.45) {
        std::cout << "  Storage ratio too high (" << totalRatio
                  << "x > 1.45x). Skipping sidecar." << std::endl;
        // Clear sidecar flag if it was set
        header.flags &= ~erwt3d::FLAG_HAS_XP_SIDECAR;
        header.reserved[21] = 0;
        pwrite(fdErwt, &header, sizeof(header), 0);
        close(fdRaw); close(fdErwt);
        return 0;
    }

    // Honor user-requested stride if it's larger (more conservative)
    if (requestedStride > stride) {
        stride = requestedStride;
        projS = ratio * rawBytes / stride;
        totalRatio = static_cast<double>(mainBytes + projS) / rawBytes;
        std::cout << "  Using requested stride=" << stride << ": " << totalRatio << "x" << std::endl;
    }

    uint32_t planeCount = (nx + stride - 1) / stride;
    uint64_t totalChunks = static_cast<uint64_t>(planeCount) * chunksPerPlane;

    std::cout << "Creating sidecar: " << xpPath << std::endl;
    std::cout << "  Stride: " << stride << ", Planes: " << planeCount
              << ", Chunks/plane: " << chunksPerPlane
              << ", Total chunks: " << totalChunks << std::endl;

    // Extract all planes via sequential scan
    std::cout << "Scanning raw to extract " << planeCount << " planes..." << std::endl;
    std::vector<std::vector<float>> planes(planeCount, std::vector<float>(planeFloats));
    for (uint64_t z = 0; z < nz; ++z) {
        uint64_t rowOff = z * rowFloats * sizeof(float);
        ssize_t rd = pread(fdRaw, row.data(), rowFloats * sizeof(float), rowOff);
        if (rd != static_cast<ssize_t>(rowFloats * sizeof(float))) {
            std::cerr << "\nError reading raw z-layer " << z << std::endl;
            close(fdRaw); close(fdErwt); return 1;
        }
        for (uint64_t pi = 0; pi < planeCount; ++pi) {
            uint64_t x = pi * stride;
            if (x >= nx) x = nx - 1;
            for (uint64_t y = 0; y < ny; ++y) {
                planes[pi][z * ny + y] = row[y * nx + x];
            }
        }
        if (z % 100 == 0) {
            std::cout << "\r  z=" << z << "/" << nz << " (" << (z * 100 / nz) << "%)" << std::flush;
        }
    }
    std::cout << "\r  z=" << nz << "/" << nz << " (100%)" << std::endl;

    close(fdRaw);

    // Create sidecar file
    int fdXp = open(xpPath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fdXp < 0) { perror("open sidecar"); close(fdErwt); return 1; }

    // Write header placeholder
    XPSidecarHeader xpHdr{};
    std::memcpy(xpHdr.magic, XPSIDECAR_MAGIC, 8);
    xpHdr.version = XPSIDECAR_VERSION;
    xpHdr.nx = nx; xpHdr.ny = ny; xpHdr.nz = nz;
    xpHdr.stride = stride;
    xpHdr.chunk_z_rows = chunkZRows;
    xpHdr.chunks_per_plane = chunksPerPlane;
    xpHdr.plane_count = planeCount;
    xpHdr.plane_floats = planeFloats;
    xpHdr.chunk_raw_bytes = chunkRawBytes;
    xpHdr.total_chunks = totalChunks;
    xpHdr.compression = 1;
    if (pwrite(fdXp, &xpHdr, sizeof(xpHdr), 0) != sizeof(xpHdr)) {
        perror("write sidecar header"); close(fdXp); close(fdErwt); return 1;
    }

    // Compress and write chunks
    std::vector<XPChunkIndex> index(totalChunks);
    uint64_t dataOffset = sizeof(xpHdr);

    std::cout << "Compressing and writing chunks..." << std::endl;
    double totalStorageBytes = compressPlanesToSidecar(
        fdXp, planes, ny, nz, stride, chunkZRows, index, dataOffset);
    if (totalStorageBytes < 0) {
        close(fdXp); close(fdErwt); return 1;
    }

    // Write index at end
    uint64_t indexOffset = dataOffset + static_cast<uint64_t>(totalStorageBytes);
    ssize_t idxBytes = static_cast<ssize_t>(totalChunks * sizeof(XPChunkIndex));
    if (pwrite(fdXp, index.data(), idxBytes, indexOffset) != idxBytes) {
        perror("write index"); close(fdXp); close(fdErwt); return 1;
    }

    // Patch header with final offsets
    xpHdr.index_offset = indexOffset;
    xpHdr.total_storage_bytes = static_cast<uint64_t>(totalStorageBytes);
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

    double finalRatio = static_cast<double>(mainBytes + totalStorageBytes) / rawBytes;
    std::cout << "Done. Sidecar: " << static_cast<uint64_t>(totalStorageBytes) / (1024 * 1024) << " MB compressed"
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

    // Extract planes via sequential scan of raw (X-Y-Z row-major, X varies fastest)
    std::vector<std::vector<float>> planes(planeCount, std::vector<float>(planeFloats));
    uint64_t rowFloats = nx * ny;
    std::vector<float> row(rowFloats);

    std::cout << "Scanning raw to extract " << planeCount << " planes..." << std::endl;
    for (uint64_t z = 0; z < nz; ++z) {
        uint64_t rowOff = z * rowFloats * sizeof(float);
        ssize_t rd = pread(fdRaw, row.data(), rowFloats * sizeof(float), rowOff);
        if (rd != static_cast<ssize_t>(rowFloats * sizeof(float))) {
            std::cerr << "\nError reading raw z-layer " << z << std::endl;
            close(fdRaw); close(fdErwt);
            return 1;
        }
        for (uint64_t pi = 0; pi < planeCount; ++pi) {
            uint64_t x = pi * stride;
            if (x >= nx) x = nx - 1;
            for (uint64_t y = 0; y < ny; ++y) {
                planes[pi][z * ny + y] = row[y * nx + x];
            }
        }
        if (z % 100 == 0) {
            std::cout << "\r  z=" << z << "/" << nz << " (" << (z * 100 / nz) << "%)" << std::flush;
        }
    }
    std::cout << "\r  z=" << nz << "/" << nz << " (100%)" << std::endl;
    close(fdRaw);

    // Write planes to erwt3d file
    for (uint64_t pi = 0; pi < planeCount; ++pi) {
        uint64_t off = xPlaneOffset + pi * planeFloats * sizeof(float);
        ssize_t wr = pwrite(fdErwt, planes[pi].data(), planeFloats * sizeof(float), off);
        if (wr != static_cast<ssize_t>(planeFloats * sizeof(float))) {
            std::cerr << "\nError writing plane " << pi << std::endl;
            close(fdErwt);
            return 1;
        }
    }

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
    }

    if (rawPath.empty() || erwtPath.empty() || nx == 0 || ny == 0 || nz == 0) {
        std::cerr << "Usage: erwt3d_precompute_x --raw data.raw --erwt3d data.erwt3d"
                  << " --nx N --ny N --nz N [options]" << std::endl;
        std::cerr << "  --mode sidecar|legacy (default: sidecar)" << std::endl;
        std::cerr << "  --stride N (sidecar: auto-decided from 1, legacy: default 1)" << std::endl;
        std::cerr << "  --chunk-z-rows N (sidecar, default: 256)" << std::endl;
        return 1;
    }

    // stride=0 means auto-decide for sidecar, default 1 for legacy
    if (mode == "sidecar") {
        uint32_t hintStride = (stride == 0) ? 1 : stride;
        return runSidecar(rawPath, erwtPath, nx, ny, nz, hintStride, chunkZRows);
    } else {
        if (stride == 0) stride = 1;
        if (stride < 1) stride = 1;
        return runLegacy(rawPath, erwtPath, nx, ny, nz, stride);
    }
}
