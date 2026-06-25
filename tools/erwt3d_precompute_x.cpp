#include "erwt3d/reader.hpp"
#include "erwt3d/format.hpp"
#include <iostream>
#include <vector>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

using erwt3d::ERWT3DHeader;

int main(int argc, char* argv[]) {
    std::string rawPath, erwtPath;
    uint64_t nx = 0, ny = 0, nz = 0;
    uint32_t stride = 1;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--raw" && i + 1 < argc) rawPath = argv[++i];
        else if (std::string(argv[i]) == "--erwt3d" && i + 1 < argc) erwtPath = argv[++i];
        else if (std::string(argv[i]) == "--nx" && i + 1 < argc) nx = std::stoull(argv[++i]);
        else if (std::string(argv[i]) == "--ny" && i + 1 < argc) ny = std::stoull(argv[++i]);
        else if (std::string(argv[i]) == "--nz" && i + 1 < argc) nz = std::stoull(argv[++i]);
        else if (std::string(argv[i]) == "--stride" && i + 1 < argc) stride = std::stoul(argv[++i]);
    }
    if (rawPath.empty() || erwtPath.empty() || nx == 0 || ny == 0 || nz == 0) {
        std::cerr << "Usage: erwt3d_precompute_x --raw data.raw --erwt3d data.erwt3d"
                  << " --nx N --ny N --nz N [--stride N]" << std::endl;
        std::cerr << "  stride: store every Nth X plane (default: 1 = all planes)" << std::endl;
        return 1;
    }
    if (stride < 1) stride = 1;

    uint64_t planeFloats = ny * nz;
    uint64_t planeBytes = planeFloats * sizeof(float);
    uint64_t planeCount = (nx + stride - 1) / stride;
    uint64_t totalPlaneBytes = planeCount * planeBytes;

    std::cout << "Pre-computing X planes from raw data..." << std::endl;
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

    // Update header
    header.flags |= (1ULL << 3);  // FLAG_HAS_X_PLANES
    header.reserved[16] = xPlaneOffset;
    header.reserved[17] = planeCount;  // number of stored planes
    header.reserved[18] = stride;      // stride value
    if (pwrite(fdErwt, &header, sizeof(header), 0) != sizeof(header)) {
        std::cerr << "Error updating header" << std::endl;
    }

    close(fdRaw);
    close(fdErwt);
    std::cout << "Done. X-plane offset: " << xPlaneOffset
              << " (" << totalPlaneBytes / (1024*1024) << " MB stored)" << std::endl;
    return 0;
}
