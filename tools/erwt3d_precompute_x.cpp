#include "erwt3d/reader.hpp"
#include "erwt3d/format.hpp"
#include "erwt3d/lz4_xp_sidecar.hpp"
#include <iostream>
#include <vector>
#include <cstring>
#include <cmath>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

using erwt3d::ERWT3DHeader;

static int runSidecar(const std::string& rawPath, const std::string& erwtPath,
                      uint64_t nx, uint64_t ny, uint64_t nz,
                      uint32_t requestedStride, uint32_t chunkZRows,
                      double storageBudget = 1.50) {
    erwt3d::Lz4XpSidecarStats stats;
    if (!erwt3d::writeLz4XpSidecar(rawPath, erwtPath, nx, ny, nz,
                                    requestedStride, chunkZRows, storageBudget, true, &stats)) {
        std::cerr << "Error: sidecar generation failed" << std::endl;
        return 1;
    }
    std::cout << "Done. Sidecar: " << stats.sidecar_bytes / (1024 * 1024) << " MB compressed"
              << ", total storage ratio: " << stats.total_storage_ratio << "x" << std::endl;
    return 0;
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
    double storageBudget = 1.50;

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
        std::cerr << "  --storage-budget X (sidecar, default: 1.50)" << std::endl;
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
