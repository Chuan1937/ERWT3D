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

static bool estimateSidecarCompression(int fdRaw, uint64_t nx, uint64_t ny, uint64_t nz,
                                       uint32_t stride, uint32_t chunkZRows,
                                       double& outRatio) {
#ifdef ERWT3D_HAVE_LZ4
    uint64_t planeFloats = ny * nz;
    uint64_t planeBytes = planeFloats * sizeof(float);
    uint64_t chunkRawBytes = ny * static_cast<uint64_t>(chunkZRows) * sizeof(float);
    uint32_t chunksPerPlane = (nz + chunkZRows - 1) / chunkZRows;

    uint32_t sampleCount = 10;
    if (nx / stride < sampleCount) sampleCount = nx / stride;
    if (sampleCount == 0) { outRatio = 1.0; return true; }

    std::vector<float> plane(planeFloats);
    std::vector<char> compBuf(LZ4_compressBound(static_cast<int>(chunkRawBytes)));

    uint64_t totalComp = 0, totalRaw = 0;
    for (uint32_t s = 0; s < sampleCount; ++s) {
        uint64_t x = (static_cast<uint64_t>(s) * nx) / sampleCount;
        if (x >= nx) x = nx - 1;
        uint64_t rawOff = x * planeBytes;
        ssize_t rd = pread(fdRaw, plane.data(), planeBytes, rawOff);
        if (rd != static_cast<ssize_t>(planeBytes)) return false;

        for (uint32_t c = 0; c < chunksPerPlane; ++c) {
            uint64_t zStart = static_cast<uint64_t>(c) * chunkZRows;
            uint64_t zEnd = std::min(zStart + chunkZRows, nz);
            uint64_t thisRawBytes = (zEnd - zStart) * ny * sizeof(float);
            const char* src = reinterpret_cast<const char*>(plane.data()) + zStart * ny;
            int cs = LZ4_compress_default(src, compBuf.data(),
                                          static_cast<int>(thisRawBytes),
                                          static_cast<int>(compBuf.size()));
            if (cs > 0) {
                totalComp += cs;
                totalRaw += thisRawBytes;
            }
        }
    }
    if (totalRaw == 0) { outRatio = 1.0; return true; }
    outRatio = static_cast<double>(totalComp) / static_cast<double>(totalRaw);
    return true;
#else
    (void)fdRaw; (void)nx; (void)ny; (void)nz; (void)stride; (void)chunkZRows;
    outRatio = 1.0;
    return false;
#endif
}

static int runSidecar(const std::string& rawPath, const std::string& erwtPath,
                      uint64_t nx, uint64_t ny, uint64_t nz,
                      uint32_t stride, uint32_t chunkZRows) {
#ifndef ERWT3D_HAVE_LZ4
    std::cerr << "Error: LZ4 support not compiled in. Sidecar mode requires LZ4." << std::endl;
    return 1;
#else
    std::string xpPath = erwtPath + ".xp";

    uint64_t planeFloats = ny * nz;
    uint64_t planeBytes = planeFloats * sizeof(float);
    uint64_t chunkRawBytes = ny * static_cast<uint64_t>(chunkZRows) * sizeof(float);
    uint32_t chunksPerPlane = (nz + chunkZRows - 1) / chunkZRows;

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
    uint64_t rawBytes = nx * ny * nz * sizeof(float);

    // Compression ratio estimation
    std::cout << "Estimating sidecar compression ratio..." << std::endl;
    double ratio;
    if (!estimateSidecarCompression(fdRaw, nx, ny, nz, stride, chunkZRows, ratio)) {
        std::cerr << "Error: compression estimation failed" << std::endl;
        close(fdRaw); close(fdErwt); return 1;
    }
    std::cout << "  Estimated compression ratio: " << ratio << "x" << std::endl;

    // Stride decision: try stride first, then stride=1 if storage allows
    double projS = ratio * rawBytes / stride;
    double totalRatio = static_cast<double>(mainBytes + projS) / rawBytes;
    std::cout << "  Projected total storage ratio (stride=" << stride
              << "): " << totalRatio << "x" << std::endl;

    if (totalRatio > 1.45) {
        if (stride > 1) {
            stride = 1;
            projS = ratio * rawBytes;
            totalRatio = static_cast<double>(mainBytes + projS) / rawBytes;
            std::cout << "  Retrying with stride=1: " << totalRatio << "x" << std::endl;
        }
        if (totalRatio > 1.45) {
            std::cout << "  Storage ratio too high (" << totalRatio
                      << "x > 1.45x). Skipping sidecar." << std::endl;
            close(fdRaw); close(fdErwt);
            return 0;
        }
    }

    uint32_t planeCount = (nx + stride - 1) / stride;
    uint64_t totalChunks = static_cast<uint64_t>(planeCount) * chunksPerPlane;

    std::cout << "Creating sidecar: " << xpPath << std::endl;
    std::cout << "  Stride: " << stride << ", Planes: " << planeCount
              << ", Chunks/plane: " << chunksPerPlane
              << ", Total chunks: " << totalChunks << std::endl;

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
    xpHdr.plane_floats = planeFloats;
    xpHdr.chunk_raw_bytes = chunkRawBytes;
    xpHdr.total_chunks = totalChunks;
    xpHdr.compression = 1;
    if (pwrite(fdXp, &xpHdr, sizeof(xpHdr), 0) != sizeof(xpHdr)) {
        perror("write sidecar header"); close(fdXp); close(fdRaw); close(fdErwt); return 1;
    }

    // Compress and write chunks
    std::vector<float> plane(planeFloats);
    std::vector<char> compBuf(LZ4_compressBound(static_cast<int>(chunkRawBytes)));
    std::vector<XPChunkIndex> index(totalChunks);

    uint64_t dataOffset = sizeof(xpHdr);
    uint64_t chunkIdx = 0;
    uint64_t totalStorageBytes = 0;
    uint64_t written = 0;

    for (uint64_t x = 0; x < nx; x += stride) {
        if (written % 10 == 0)
            std::cout << "\r  " << written << "/" << planeCount
                      << " (" << (written * 100 / planeCount) << "%)" << std::flush;

        uint64_t rawOff = x * planeBytes;
        ssize_t rd = pread(fdRaw, plane.data(), planeBytes, rawOff);
        if (rd != static_cast<ssize_t>(planeBytes)) {
            std::cerr << "\nError reading raw X[" << x << "]" << std::endl;
            close(fdXp); close(fdRaw); close(fdErwt); return 1;
        }

        for (uint32_t c = 0; c < chunksPerPlane; ++c) {
            uint64_t zStart = static_cast<uint64_t>(c) * chunkZRows;
            uint64_t zEnd = std::min(zStart + chunkZRows, nz);
            uint64_t thisRawBytes = (zEnd - zStart) * ny * sizeof(float);
            const char* src = reinterpret_cast<const char*>(plane.data()) + zStart * ny;

            int cs = LZ4_compress_default(src, compBuf.data(),
                                          static_cast<int>(thisRawBytes),
                                          static_cast<int>(compBuf.size()));
            if (cs <= 0) {
                std::cerr << "\nLZ4 compress failed for plane " << written
                          << " chunk " << c << std::endl;
                close(fdXp); close(fdRaw); close(fdErwt); return 1;
            }

            index[chunkIdx].chunk_offset = dataOffset + totalStorageBytes;
            index[chunkIdx].compressed_size = static_cast<uint32_t>(cs);
            index[chunkIdx].raw_size = static_cast<uint32_t>(thisRawBytes);
            totalStorageBytes += cs;

            if (pwrite(fdXp, compBuf.data(), cs, index[chunkIdx].chunk_offset) != cs) {
                perror("write chunk"); close(fdXp); close(fdRaw); close(fdErwt); return 1;
            }
            ++chunkIdx;
        }
        ++written;
    }
    std::cout << "\r  " << written << "/" << planeCount << " (100%)" << std::endl;

    // Write index at end
    uint64_t indexOffset = dataOffset + totalStorageBytes;
    ssize_t idxBytes = static_cast<ssize_t>(totalChunks * sizeof(XPChunkIndex));
    if (pwrite(fdXp, index.data(), idxBytes, indexOffset) != idxBytes) {
        perror("write index"); close(fdXp); close(fdRaw); close(fdErwt); return 1;
    }

    // Patch header with final offsets
    xpHdr.index_offset = indexOffset;
    xpHdr.total_storage_bytes = totalStorageBytes;
    if (pwrite(fdXp, &xpHdr, sizeof(xpHdr), 0) != sizeof(xpHdr)) {
        perror("patch sidecar header"); close(fdXp); close(fdRaw); close(fdErwt); return 1;
    }

    // Update main ERWT3D header
    header.flags |= erwt3d::FLAG_HAS_XP_SIDECAR;
    header.reserved[21] = 1;
    if (pwrite(fdErwt, &header, sizeof(header), 0) != sizeof(header)) {
        std::cerr << "Warning: failed to update main header" << std::endl;
    }

    close(fdXp);
    close(fdRaw);
    close(fdErwt);

    double finalRatio = static_cast<double>(mainBytes + totalStorageBytes) / rawBytes;
    std::cout << "Done. Sidecar: " << totalStorageBytes / (1024 * 1024) << " MB compressed"
              << ", total storage ratio: " << finalRatio << "x" << std::endl;
    return 0;
#endif
}

static int runLegacy(const std::string& rawPath, const std::string& erwtPath,
                     uint64_t nx, uint64_t ny, uint64_t nz, uint32_t stride) {
    uint64_t planeFloats = ny * nz;
    uint64_t planeBytes = planeFloats * sizeof(float);
    uint64_t planeCount = (nx + stride - 1) / stride;
    uint64_t totalPlaneBytes = planeCount * planeBytes;

    std::cout << "Pre-computing X planes from raw data (legacy mode)..." << std::endl;
    std::cout << "  Raw: " << rawPath << std::endl;
    std::cout << "  ERWT3D: " << erwtPath << std::endl;
    std::cout << "  Stride: " << stride << " (every " << stride << "th X value)" << std::endl;
    std::cout << "  Planes: " << planeCount << " x " << planeBytes / (1024*1024) << " MB"
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

    std::vector<float> plane(planeFloats);
    uint64_t written = 0;

    for (uint64_t x = 0; x < nx; x += stride) {
        if (written % 10 == 0)
            std::cout << "\r  " << written << "/" << planeCount
                      << " (" << (written * 100 / planeCount) << "%)" << std::flush;
        uint64_t rawOff = x * planeBytes;
        ssize_t rd = pread(fdRaw, plane.data(), planeBytes, rawOff);
        if (rd != static_cast<ssize_t>(planeBytes)) {
            std::cerr << "\nError reading raw X[" << x << "]: " << rd << std::endl;
            close(fdRaw); close(fdErwt);
            return 1;
        }
        uint64_t off = xPlaneOffset + written * planeBytes;
        if (pwrite(fdErwt, plane.data(), planeBytes, off) != static_cast<ssize_t>(planeBytes)) {
            std::cerr << "\nError writing plane " << written << std::endl;
            close(fdRaw); close(fdErwt);
            return 1;
        }
        ++written;
    }
    std::cout << "\r  " << written << "/" << planeCount << " (100%)" << std::endl;

    header.flags |= (1ULL << 3);
    header.reserved[16] = xPlaneOffset;
    header.reserved[17] = planeCount;
    header.reserved[18] = stride;
    if (pwrite(fdErwt, &header, sizeof(header), 0) != sizeof(header)) {
        std::cerr << "Error updating header" << std::endl;
    }

    close(fdRaw);
    close(fdErwt);
    std::cout << "Done. X-plane offset: " << xPlaneOffset
              << " (" << totalPlaneBytes / (1024*1024) << " MB stored)" << std::endl;
    return 0;
}

int main(int argc, char* argv[]) {
    std::string rawPath, erwtPath;
    uint64_t nx = 0, ny = 0, nz = 0;
    uint32_t stride = 2;
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
        std::cerr << "  --stride N (sidecar default: 2, legacy default: 1)" << std::endl;
        std::cerr << "  --chunk-z-rows N (sidecar, default: 256)" << std::endl;
        return 1;
    }

    if (stride < 1) stride = 1;

    if (mode == "sidecar") {
        return runSidecar(rawPath, erwtPath, nx, ny, nz, stride, chunkZRows);
    } else {
        if (stride == 2) stride = 1;
        return runLegacy(rawPath, erwtPath, nx, ny, nz, stride);
    }
}
