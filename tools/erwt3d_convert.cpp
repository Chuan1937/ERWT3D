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
                                      double& outRatio, int numSlabs = 4) {
    uint64_t sgX = (nx + sx - 1) / sx;
    uint64_t sgY = (ny + sy - 1) / sy;
    uint64_t sgZ = (nz + sz - 1) / sz;
    uint64_t sbFloats = (uint64_t)sx * sy * sz;
    uint64_t sbBytes = sbFloats * sizeof(float);

    int fd = open(rawPath.c_str(), O_RDONLY);
    if (fd < 0) return false;

    uint64_t rawRowBytes = nx * sizeof(float);
    uint64_t rawSliceBytes = rawRowBytes * ny;
    std::vector<float> slabBuf(sbFloats * sgX * sgY);
    std::vector<uint8_t> leafBuf(sbBytes);
    int compBufSize = LZ4_compressBound(static_cast<int>(sbBytes));
    std::vector<uint8_t> compBuf(compBufSize);

    uint64_t totalRaw = 0, totalComp = 0;
    int actualSamples = 0;
    int maxPerSlab = 2048 / numSlabs + 1;

    std::vector<uint64_t> slabZs;
    int step = std::max<int>(1, static_cast<int>(sgZ) / numSlabs);
    for (int i = 0; i < numSlabs && (uint64_t)i * step < sgZ; ++i)
        slabZs.push_back(i * step);

    for (uint64_t szStart : slabZs) {
        int slabSamples = 0;
        uint64_t zStart = szStart * sz;
        uint64_t zEnd = std::min(zStart + sz, nz);
        uint64_t zRows = zEnd - zStart;

        for (uint64_t z = zStart; z < zEnd; ++z) {
            if (z >= nz) break;
            uint64_t rawOff = z * rawSliceBytes;
            ssize_t rd = pread(fd, slabBuf.data() + (z - zStart) * (nx * ny),
                               rawSliceBytes, rawOff);
            if (rd < 0) { close(fd); return false; }
        }

        for (uint64_t syi = 0; syi < sgY; ++syi) {
            for (uint64_t sxi = 0; sxi < sgX; ++sxi) {
                uint64_t startX = sxi * sx, startY = syi * sy;
                std::vector<float> sbBuf(sbFloats);

                for (uint64_t zz = 0; zz < zRows; ++zz) {
                    for (uint64_t yy = 0; yy < sy; ++yy) {
                        uint64_t gy = startY + yy;
                        if (gy >= ny) break;
                        uint64_t srcOff = (zz * ny + gy) * nx + startX;
                        uint64_t vx = std::min<uint64_t>(sx, nx - startX);
                        const float* src = slabBuf.data() + srcOff;
                        float* dst = sbBuf.data() + (zz * sy + yy) * sx;
                        std::memcpy(dst, src, vx * sizeof(float));
                    }
                }

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

                int compSize = LZ4_compress_default(
                    reinterpret_cast<const char*>(leafBuf.data()),
                    reinterpret_cast<char*>(compBuf.data()),
                    static_cast<int>(sbBytes), compBufSize);

                if (compSize > 0) {
                    totalRaw += sbBytes;
                    totalComp += compSize;
                    actualSamples++;
                    slabSamples++;
                }
                if (slabSamples >= maxPerSlab) break;
            }
            if (actualSamples >= 2048) break;
        }
        if (actualSamples >= 2048) break;
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
    std::cerr << "  --converter NAME    Conversion engine: zslab (default)" << std::endl;
    std::cerr << "  --physical-order O  Output order: v05-yzx (default) or zyx" << std::endl;
    std::cerr << "  --scratch-dir DIR   Temporary bucket directory for compressed v05-yzx output" << std::endl;
    std::cerr << "  --x-sidecar         Build compressed X-sidecar during the same raw read pass" << std::endl;
    std::cerr << "  --x-sidecar-stride N Store every Nth X plane (default: 1)" << std::endl;
    std::cerr << "  --x-sidecar-storage-budget X  Keep sidecar only if total ratio <= X (default: 1.45)" << std::endl;
    std::cerr << "  --compress on|off|auto  Enable lz4 (default: on, auto estimates ratio)" << std::endl;
    std::cerr << "  --compress-threshold X   Auto skip if ratio >= X (default: 0.95)" << std::endl;
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
    std::string compressMode = "on";
    double compressThreshold = 0.95;
    std::string converter = "zslab";
    erwt3d::PhysicalOrder physicalOrder = erwt3d::PhysicalOrder::V05_YZX;
    std::string scratchDir;
    bool xSidecar = false;
    uint32_t xSidecarStride = 1;
    double xSidecarStorageBudget = 1.45;
    
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
        } else if (std::strcmp(argv[i], "--compress") == 0 && i + 1 < argc) {
            compressMode = argv[++i];
        } else if (std::strcmp(argv[i], "--compress") == 0) {
            compressMode = "on";  // bare --compress = on
        } else if (std::strcmp(argv[i], "--compress-threshold") == 0 && i + 1 < argc) {
            compressThreshold = std::stod(argv[++i]);
        } else if (std::strcmp(argv[i], "--layout") == 0 && i + 1 < argc) {
            std::string layout = argv[++i];
            if (layout == "v05" || layout == "v05-yzx" || layout == "xyz") {
                physicalOrder = erwt3d::PhysicalOrder::V05_YZX;
            } else if (layout == "zyx" || layout == "zyx-experimental") {
                physicalOrder = erwt3d::PhysicalOrder::ZYX;
            } else {
                std::cerr << "Error: unknown layout: " << layout
                          << " (valid: v05, v05-yzx, zyx-experimental)" << std::endl;
                return 1;
            }
        } else if (std::strcmp(argv[i], "--converter") == 0 && i + 1 < argc) {
            converter = argv[++i];
        } else if (std::strcmp(argv[i], "--physical-order") == 0 && i + 1 < argc) {
            std::string order = argv[++i];
            if (order == "v05" || order == "v05-yzx" || order == "yzx") {
                physicalOrder = erwt3d::PhysicalOrder::V05_YZX;
            } else if (order == "zyx") {
                physicalOrder = erwt3d::PhysicalOrder::ZYX;
            } else {
                std::cerr << "Error: unknown physical order: " << order
                          << " (valid: v05-yzx, zyx)" << std::endl;
                return 1;
            }
        } else if (std::strcmp(argv[i], "--scratch-dir") == 0 && i + 1 < argc) {
            scratchDir = argv[++i];
        } else if (std::strcmp(argv[i], "--x-sidecar") == 0) {
            xSidecar = true;
        } else if (std::strcmp(argv[i], "--x-sidecar-stride") == 0 && i + 1 < argc) {
            xSidecarStride = std::stoul(argv[++i]);
        } else if (std::strcmp(argv[i], "--x-sidecar-storage-budget") == 0 && i + 1 < argc) {
            xSidecarStorageBudget = std::stod(argv[++i]);
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

    if (converter != "zslab") {
        std::cerr << "Error: only --converter zslab is available on this branch" << std::endl;
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

        if (xSidecar) {
            if (xSidecarStride == 0) {
                std::cerr << "Error: --x-sidecar-stride must be greater than zero" << std::endl;
                return 1;
            }
            if (xSidecarStorageBudget <= 0.0) {
                std::cerr << "Error: --x-sidecar-storage-budget must be greater than zero" << std::endl;
                return 1;
            }
            std::cout << "X-sidecar: integrated single-pass build, stride=" << xSidecarStride
                      << ", storage budget=" << xSidecarStorageBudget << "x" << std::endl;
        }

#ifdef ERWT3D_HAVE_LZ4
        if (compressMode == "auto") {
            double estRatio = 1.0;
            if (estimateCompressionRatio(inputPath, nx, ny, nz,
                                         superSize, superSize, superSize,
                                         leafSize, leafSize, leafSize,
                                         estRatio, 8)) {
                std::cout << "Compression estimation (up to 2048 superblocks): ratio="
                          << std::fixed << std::setprecision(3) << estRatio << "x";
                if (estRatio >= compressThreshold) {
                    std::cout << " >= " << compressThreshold << " — skipping compression" << std::endl;
                    compress = false;
                } else {
                    std::cout << " < " << compressThreshold << " — enabling compression" << std::endl;
                    compress = true;
                }
            } else {
                std::cerr << "Compression estimation failed, falling back to compress=on" << std::endl;
                compress = true;
            }
        } else if (compressMode == "on") {
            compress = true;
        } else if (compressMode == "off") {
            compress = false;
        } else {
            std::cerr << "Error: --compress must be on, off, or auto" << std::endl;
            return 1;
        }
        
        if (compress) {
            std::cout << "Compression: enabled (lz4, per-block decision)" << std::endl;
        }
#else
        (void)compressMode; (void)compressThreshold;
#endif
        
        if (!erwt3d::writeERWT3DFromFile(outputPath, inputPath, nx, ny, nz,
                                         superSize, superSize, superSize,
                                         leafSize, leafSize, leafSize,
                                         numThreads, memoryLimitMB,
                                         panelAxis, panelStride, compress,
                                         physicalOrder, scratchDir,
                                         xSidecar, xSidecarStride, xSidecarStorageBudget)) {
            std::cerr << "Error: Failed to convert raw to ERWT3D" << std::endl;
            return 1;
        }
        
        std::cout << "Conversion complete: " << outputPath << std::endl;
    }
    
    return 0;
}
