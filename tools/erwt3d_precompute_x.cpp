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
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--raw" && i + 1 < argc) rawPath = argv[++i];
        else if (std::string(argv[i]) == "--erwt3d" && i + 1 < argc) erwtPath = argv[++i];
        else if (std::string(argv[i]) == "--nx" && i + 1 < argc) nx = std::stoull(argv[++i]);
        else if (std::string(argv[i]) == "--ny" && i + 1 < argc) ny = std::stoull(argv[++i]);
        else if (std::string(argv[i]) == "--nz" && i + 1 < argc) nz = std::stoull(argv[++i]);
    }
    if (rawPath.empty() || erwtPath.empty() || nx == 0 || ny == 0 || nz == 0) {
        std::cerr << "Usage: erwt3d_precompute_x --raw data.raw --erwt3d data.erwt3d --nx N --ny N --nz N" << std::endl;
        return 1;
    }

    uint64_t planeFloats = ny * nz;
    uint64_t planeBytes = planeFloats * sizeof(float);
    uint64_t totalPlaneBytes = nx * planeBytes;

    std::cout << "Pre-computing X planes from raw data..." << std::endl;
    std::cout << "  Raw: " << rawPath << std::endl;
    std::cout << "  ERWT3D: " << erwtPath << std::endl;
    std::cout << "  Plane: " << planeBytes / (1024*1024) << " MB x " << nx << std::endl;

    // Open raw file (X-Y-Z order: X contiguous)
    int fdRaw = open(rawPath.c_str(), O_RDONLY);
    if (fdRaw < 0) { perror("open raw"); return 1; }

    // Open erwt3d file for appending
    int fdErwt = open(erwtPath.c_str(), O_RDWR);
    if (fdErwt < 0) { perror("open erwt3d"); close(fdRaw); return 1; }

    // Get erwt3d file size
    struct stat st;
    fstat(fdErwt, &st);
    uint64_t xPlaneOffset = st.st_size;

    // Read and update header
    ERWT3DHeader header;
    pread(fdErwt, &header, sizeof(header), 0);

    // Buffer for one X plane
    std::vector<float> plane(planeFloats);

    // Read each X slice from raw (contiguous in X-Y-Z order)
    for (uint64_t x = 0; x < nx; ++x) {
        if (x % 10 == 0)
            std::cout << "\r  " << x << "/" << nx << " (" << (x * 100 / nx) << "%)" << std::flush;
        // In X-Y-Z order: offset = x * ny * nz * sizeof(float)
        uint64_t rawOff = x * planeBytes;
        ssize_t rd = pread(fdRaw, plane.data(), planeBytes, rawOff);
        if (rd != static_cast<ssize_t>(planeBytes)) {
            std::cerr << "\nError reading raw X[" << x << "]: " << rd << std::endl;
            close(fdRaw); close(fdErwt);
            return 1;
        }
        uint64_t off = xPlaneOffset + x * planeBytes;
        if (pwrite(fdErwt, plane.data(), planeBytes, off) != static_cast<ssize_t>(planeBytes)) {
            std::cerr << "\nError writing X[" << x << "]" << std::endl;
            close(fdRaw); close(fdErwt);
            return 1;
        }
    }
    std::cout << "\r  " << nx << "/" << nx << " (100%)" << std::endl;

    // Update header
    header.flags |= (1ULL << 3);  // FLAG_HAS_X_PLANES
    header.reserved[16] = xPlaneOffset;
    header.reserved[17] = nx;
    if (pwrite(fdErwt, &header, sizeof(header), 0) != sizeof(header)) {
        std::cerr << "Error updating header" << std::endl;
    }

    close(fdRaw);
    close(fdErwt);
    std::cout << "Done. X-plane offset: " << xPlaneOffset
              << " (" << totalPlaneBytes / (1024*1024*1024) << " GB)" << std::endl;
    return 0;
}
