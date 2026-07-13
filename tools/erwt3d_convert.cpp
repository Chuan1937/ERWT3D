#include "erwt3d/writer.hpp"
#include "erwt3d/reader.hpp"
#include "erwt3d/morton.hpp"
#include <iostream>
#include <iomanip>
#include <string>
#include <cstring>
#include <cmath>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>

#ifdef ERWT3D_HAVE_LZ4
#include <lz4.h>

static bool estimateCompressionRatio(const std::string& rawPath,
                                      uint64_t nx, uint64_t ny, uint64_t nz,
                                      uint32_t sx, uint32_t sy, uint32_t sz,
                                      uint32_t lx, uint32_t ly, uint32_t lz,
                                      double& outRatio, int numSamples = 200) {
    uint64_t sgX = (nx + sx - 1) / sx;
    uint64_t sgY = (ny + sy - 1) / sy;
    uint64_t sgZ = (nz + sz - 1) / sz;
    uint64_t totalSB = sgX * sgY * sgZ;
    uint64_t sbFloats = (uint64_t)sx * sy * sz;
    uint64_t sbBytes = sbFloats * sizeof(float);

    int fd = open(rawPath.c_str(), O_RDONLY);
    if (fd < 0) return false;

    std::vector<float> sbBuf(sbFloats);
    std::vector<uint8_t> leafBuf(sbBytes);
    int compBufSize = LZ4_compressBound(static_cast<int>(sbBytes));
    std::vector<uint8_t> compBuf(compBufSize);

    uint64_t totalRaw = 0, totalComp = 0;
    int actualSamples = 0;
    uint64_t step = std::max<uint64_t>(1, totalSB / numSamples);

    for (uint64_t si = 0; si < totalSB && actualSamples < numSamples; si += step) {
        uint64_t szi = si / (sgY * sgX);
        uint64_t rem = si % (sgY * sgX);
        uint64_t syi = rem / sgX;
        uint64_t sxi = rem % sgX;

        // Read raw data for this superblock
        std::memset(sbBuf.data(), 0, sbBytes);
        uint64_t startX = sxi * sx, startY = syi * sy, startZ = szi * sz;
        for (uint64_t z = 0; z < sz; ++z) {
            uint64_t gz = startZ + z; if (gz >= nz) break;
            for (uint64_t y = 0; y < sy; ++y) {
                uint64_t gy = startY + y; if (gy >= ny) break;
                uint64_t rawOff = ((gz * ny + gy) * nx + startX) * sizeof(float);
                uint64_t vx = std::min<uint64_t>(sx, nx - startX);
                ssize_t rd = pread(fd, sbBuf.data() + (z * sy + y) * sx, vx * sizeof(float), rawOff);
                if (rd < 0) { close(fd); return false; }
            }
        }

        // Convert to leaf order (same as writeLeavesToBuffer)
        uint64_t totalLeafs = (uint64_t)(sx/lx) * (sy/ly) * (sz/lz);
        size_t pos = 0;
        for (uint64_t j = 0; j < totalLeafs; ++j) {
            uint32_t plx, ply, plz;
            erwt3d::unmorton3D(j, plx, ply, plz);
            if (plx >= sx/lx || ply >= sy/ly || plz >= sz/lz) continue;
            uint64_t bx = plx*lx, by = ply*ly, bz = plz*lz;
            float* leaf = reinterpret_cast<float*>(leafBuf.data() + pos);
            for (uint64_t zz = 0; zz < lz; ++zz)
                for (uint64_t yy = 0; yy < ly; ++yy)
                    for (uint64_t xx = 0; xx < lx; ++xx)
                        leaf[(zz*ly+yy)*lx+xx] = sbBuf[((bz+zz)*sy+(by+yy))*sx+(bx+xx)];
            pos += lx*ly*lz*sizeof(float);
        }

        // Compress
        int compSize = LZ4_compress_default(
            reinterpret_cast<const char*>(leafBuf.data()),
            reinterpret_cast<char*>(compBuf.data()),
            static_cast<int>(sbBytes), compBufSize);

        if (compSize > 0) {
            totalRaw += sbBytes;
            totalComp += compSize;
            actualSamples++;
        }
    }

    close(fd);

    if (actualSamples == 0) return false;
    outRatio = static_cast<double>(totalComp) / totalRaw;
    return true;
}
#endif

void printUsage(const char* progName) {
    std::cerr << "Usage:" << std::endl;
    std::cerr << "  Convert raw to ERWT3D:" << std::endl;
    std::cerr << "    " << progName << " --input data.raw --output data.erwt3d --nx N --ny N --nz N [options]" << std::endl;
    std::cerr << std::endl;
    std::cerr << "  Convert ERWT3D to raw:" << std::endl;
    std::cerr << "    " << progName << " --input data.erwt3d --output restored.raw --to-raw [options]" << std::endl;
    std::cerr << std::endl;
    std::cerr << "Options:" << std::endl;
    std::cerr << "  --nx N              X dimension (required for raw->erwt3d)" << std::endl;
    std::cerr << "  --ny N              Y dimension (required for raw->erwt3d)" << std::endl;
    std::cerr << "  --nz N              Z dimension (required for raw->erwt3d)" << std::endl;
    std::cerr << "  --dtype TYPE        Data type (default: float32)" << std::endl;
    std::cerr << "  --layout LAYOUT     Input layout (default: xyz)" << std::endl;
    std::cerr << "  --threads N         Number of threads (default: 1)" << std::endl;
    std::cerr << "  --memory-limit-mb N Memory limit in MB (default: 2048)" << std::endl;
    std::cerr << "  --super-size N      Superblock size (default: 64)" << std::endl;
    std::cerr << "  --leaf-size N       Leaf block size (default: 4)" << std::endl;
    std::cerr << "  --panel-axis x       Enable X micro-panels (only x supported)" << std::endl;
    std::cerr << "  --panel-stride N     Store every Nth local X plane (must divide super-size)" << std::endl;
}

int main(int argc, char* argv[]) {
    std::string inputPath;
    std::string outputPath;
    uint64_t nx = 0, ny = 0, nz = 0;
    bool toRaw = false;
    int numThreads = 1;
    size_t memoryLimitMB = 2048;
    uint32_t superSize = 64;
    uint32_t leafSize = 4;
    uint32_t panelAxis = 0;
    uint32_t panelStride = 0;
    bool compress = false;
    
    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            inputPath = argv[++i];
        } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            outputPath = argv[++i];
        } else if (std::strcmp(argv[i], "--nx") == 0 && i + 1 < argc) {
            nx = std::stoull(argv[++i]);
        } else if (std::strcmp(argv[i], "--ny") == 0 && i + 1 < argc) {
            ny = std::stoull(argv[++i]);
        } else if (std::strcmp(argv[i], "--nz") == 0 && i + 1 < argc) {
            nz = std::stoull(argv[++i]);
        } else if (std::strcmp(argv[i], "--to-raw") == 0) {
            toRaw = true;
        } else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            numThreads = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--memory-limit-mb") == 0 && i + 1 < argc) {
            memoryLimitMB = std::stoul(argv[++i]);
        } else if (std::strcmp(argv[i], "--super-size") == 0 && i + 1 < argc) {
            superSize = std::stoul(argv[++i]);
        } else if (std::strcmp(argv[i], "--leaf-size") == 0 && i + 1 < argc) {
            leafSize = std::stoul(argv[++i]);
        } else if (std::strcmp(argv[i], "--panel-axis") == 0 && i + 1 < argc) {
            std::string ax = argv[++i];
            if (ax == "x" || ax == "X") panelAxis = 0;
            else if (ax == "y" || ax == "Y" || ax == "z" || ax == "Z") {
                std::cerr << "Error: only --panel-axis x is currently implemented" << std::endl;
                return 1;
            } else {
                std::cerr << "Error: unknown --panel-axis: " << ax << " (valid: x)" << std::endl;
                return 1;
            }
        } else if (std::strcmp(argv[i], "--panel-stride") == 0 && i + 1 < argc) {
            panelStride = std::stoul(argv[++i]);
        } else if (std::strcmp(argv[i], "--compress") == 0) {
            compress = true;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }
    
    if (inputPath.empty() || outputPath.empty()) {
        std::cerr << "Error: --input and --output are required" << std::endl;
        printUsage(argv[0]);
        return 1;
    }
    
    if (toRaw) {
        // Convert ERWT3D to raw
        std::cout << "Converting ERWT3D to raw..." << std::endl;
        erwt3d::ERWT3DReader reader(inputPath);
        
        if (!reader.readFullToFile(outputPath, numThreads, memoryLimitMB)) {
            std::cerr << "Error: Failed to convert ERWT3D to raw" << std::endl;
            return 1;
        }
        
        std::cout << "Conversion complete: " << outputPath << std::endl;
    } else {
        // Convert raw to ERWT3D
        if (nx == 0 || ny == 0 || nz == 0) {
            std::cerr << "Error: --nx, --ny, --nz are required for raw to ERWT3D conversion" << std::endl;
            printUsage(argv[0]);
            return 1;
        }
        
        std::cout << "Converting raw to ERWT3D..." << std::endl;
        std::cout << "Dimensions: " << nx << " x " << ny << " x " << nz << std::endl;

#ifdef ERWT3D_HAVE_LZ4
        if (compress) {
            std::cout << "Compression ratio estimation disabled; each superblock will choose lz4 or raw storage." << std::endl;
        }
#endif
        
        if (!erwt3d::writeERWT3DFromFile(outputPath, inputPath, nx, ny, nz,
                                         superSize, superSize, superSize,
                                         leafSize, leafSize, leafSize,
                                         numThreads, memoryLimitMB,
                                         panelAxis, panelStride, compress)) {
            std::cerr << "Error: Failed to convert raw to ERWT3D" << std::endl;
            return 1;
        }
        
        std::cout << "Conversion complete: " << outputPath << std::endl;
    }
    
    return 0;
}
