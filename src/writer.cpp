#include "erwt3d/writer.hpp"
#include "erwt3d/morton.hpp"
#include "erwt3d/raw_layout.hpp"
#include <fstream>
#include <vector>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#ifdef ERWT3D_HAVE_LZ4
#include <lz4.h>
#endif

namespace erwt3d {

static void writeLeaves(std::ofstream& file, const ERWT3DHeader& header,
                        const std::vector<float>& superBuffer) {
    uint64_t leafBytes = getLeafBytes(header);
    std::vector<float> leafBuffer(header.leaf_x * header.leaf_y * header.leaf_z);
    uint64_t totalLeafs = getTotalLeafsPerSuper(header);
    uint64_t superY = header.super_y;
    for (uint64_t j = 0; j < totalLeafs; ++j) {
        uint32_t lx, ly, lz; unmorton3D(j, lx, ly, lz);
        if (lx >= getLeafsPerSuperX(header) || ly >= getLeafsPerSuperY(header) || lz >= getLeafsPerSuperZ(header)) continue;
        uint64_t bx = lx*header.leaf_x, by = ly*header.leaf_y, bz = lz*header.leaf_z;
        for (uint64_t z=0; z<header.leaf_z; ++z)
            for (uint64_t y=0; y<header.leaf_y; ++y)
                for (uint64_t x=0; x<header.leaf_x; ++x)
                    leafBuffer[(z*header.leaf_y+y)*header.leaf_x+x] =
                        superBuffer[((bz+z)*superY+(by+y))*header.super_x+(bx+x)];
        file.write(reinterpret_cast<const char*>(leafBuffer.data()), leafBytes);
    }
}

static void writeLeavesToBuffer(uint8_t* out, const ERWT3DHeader& header,
                                const std::vector<float>& superBuffer) {
    uint64_t leafBytes = getLeafBytes(header);
    uint64_t totalLeafs = getTotalLeafsPerSuper(header);
    uint64_t superY = header.super_y;
    size_t pos = 0;
    for (uint64_t j = 0; j < totalLeafs; ++j) {
        uint32_t lx, ly, lz; unmorton3D(j, lx, ly, lz);
        if (lx >= getLeafsPerSuperX(header) || ly >= getLeafsPerSuperY(header) || lz >= getLeafsPerSuperZ(header)) continue;
        uint64_t bx = lx*header.leaf_x, by = ly*header.leaf_y, bz = lz*header.leaf_z;
        float* leaf = reinterpret_cast<float*>(out + pos);
        for (uint64_t z=0; z<header.leaf_z; ++z)
            for (uint64_t y=0; y<header.leaf_y; ++y)
                for (uint64_t x=0; x<header.leaf_x; ++x)
                    leaf[(z*header.leaf_y+y)*header.leaf_x+x] =
                        superBuffer[((bz+z)*superY+(by+y))*header.super_x+(bx+x)];
        pos += leafBytes;
    }
}

static bool compressAndWrite(const uint8_t* rawData, uint64_t rawSize,
                              uint8_t* compBuf, int compBufCapacity,
                              std::ofstream& outFile,
                              std::vector<CompressedBlockIndex>& index) {
    CompressedBlockIndex entry;
#ifdef ERWT3D_HAVE_LZ4
    int compSize = LZ4_compress_default(
        reinterpret_cast<const char*>(rawData),
        reinterpret_cast<char*>(compBuf),
        static_cast<int>(rawSize),
        compBufCapacity);
    if (compSize > 0 && static_cast<uint64_t>(compSize) < rawSize * 95 / 100) {
        entry.file_offset = static_cast<uint64_t>(outFile.tellp());
        entry.compressed_size = static_cast<uint32_t>(compSize);
        entry.is_compressed = 1;
        std::memset(entry.padding, 0, sizeof(entry.padding));
        outFile.write(reinterpret_cast<const char*>(compBuf), compSize);
    } else
#endif
    {
        entry.file_offset = static_cast<uint64_t>(outFile.tellp());
        entry.compressed_size = static_cast<uint32_t>(rawSize);
        entry.is_compressed = 0;
        std::memset(entry.padding, 0, sizeof(entry.padding));
        outFile.write(reinterpret_cast<const char*>(rawData), rawSize);
    }
    index.push_back(entry);
    return true;
}

static void fillSuperBuffer(std::vector<float>& sb, const float* rawData,
                            uint64_t nx, uint64_t ny, uint64_t nz,
                            uint64_t sx, uint64_t sy, uint64_t sz,
                            uint64_t superX, uint64_t superY, uint64_t superZ) {
    std::memset(sb.data(), 0, superX*superY*superZ*sizeof(float));
    uint64_t startX = sx*superX, startY = sy*superY, startZ = sz*superZ;
    for (uint64_t z = 0; z < superZ; ++z) {
        uint64_t gz = startZ+z; if (gz >= nz) break;
        for (uint64_t y = 0; y < superY; ++y) {
            uint64_t gy = startY+y; if (gy >= ny) break;
            for (uint64_t x = 0; x < superX; ++x) {
                uint64_t gx = startX+x; if (gx >= nx) break;
                sb[(z*superY+y)*superX+x] =
                    rawData[rawOffsetZFastest(gx, gy, gz, ny, nz)];
            }
        }
    }
}

static void fillSuperBufferFromFile(std::vector<float>& sb, std::ifstream& inFile,
                                     uint64_t nx, uint64_t ny, uint64_t nz,
                                     uint64_t sx, uint64_t sy, uint64_t sz,
                                     uint64_t superX, uint64_t superY, uint64_t superZ) {
    std::memset(sb.data(), 0, superX*superY*superZ*sizeof(float));
    uint64_t startX = sx*superX, startY = sy*superY, startZ = sz*superZ;
    for (uint64_t z = 0; z < superZ; ++z) {
        uint64_t gz = startZ+z; if (gz >= nz) break;
        for (uint64_t y = 0; y < superY; ++y) {
            uint64_t gy = startY+y; if (gy >= ny) break;
            uint64_t vx = std::min(static_cast<uint64_t>(superX), nx-startX);
            for (uint64_t x = 0; x < vx; ++x) {
                uint64_t gx = startX + x;
                uint64_t foff = rawOffsetBytesZFastest(gx, gy, gz, ny, nz);
                inFile.seekg(foff); inFile.clear();
                inFile.read(reinterpret_cast<char*>(&sb[(z*superY+y)*superX+x]), sizeof(float));
            }
        }
    }
}

// mmap version: read from memory-mapped file using the official Z-fastest layout.
static void fillSuperBufferFromMmap(std::vector<float>& sb, const float* rawData,
                                     uint64_t nx, uint64_t ny, uint64_t nz,
                                     uint64_t sx, uint64_t sy, uint64_t sz,
                                     uint64_t superX, uint64_t superY, uint64_t superZ) {
    std::memset(sb.data(), 0, superX*superY*superZ*sizeof(float));
    uint64_t startX = sx*superX, startY = sy*superY, startZ = sz*superZ;
    for (uint64_t z = 0; z < superZ; ++z) {
        uint64_t gz = startZ+z; if (gz >= nz) break;
        for (uint64_t y = 0; y < superY; ++y) {
            uint64_t gy = startY+y; if (gy >= ny) break;
            uint64_t vx = std::min(static_cast<uint64_t>(superX), nx-startX);
            for (uint64_t x = 0; x < vx; ++x) {
                uint64_t gx = startX + x;
                sb[(z*superY+y)*superX+x] = rawData[rawOffsetZFastest(gx, gy, gz, ny, nz)];
            }
        }
    }
}

// Sequential read strategy: read the official Z-fastest raw file one X-slab at a
// time. Each X-slab contains a contiguous range of X planes, so every pread is
// sequential and large, which is optimal for HDD.
static bool writeERWT3DFromFileSequential(const std::string& outputPath,
                                           const std::string& inputPath,
                                           uint64_t nx, uint64_t ny, uint64_t nz,
                                           uint32_t superX, uint32_t superY, uint32_t superZ,
                                           uint32_t leafX, uint32_t leafY, uint32_t leafZ,
                                           int numThreads, size_t memoryLimitMB,
                                           uint32_t panelAxis, uint32_t panelStride,
                                           bool compress = false) {
    ERWT3DHeader header;
    initHeader(header);
    header.nx=nx; header.ny=ny; header.nz=nz;
    header.super_x=superX; header.super_y=superY; header.super_z=superZ;
    header.leaf_x=leafX; header.leaf_y=leafY; header.leaf_z=leafZ;

    uint64_t sgX=getSuperGridX(header), sgY=getSuperGridY(header), sgZ=getSuperGridZ(header);
    uint64_t totalSB=sgX*sgY*sgZ, sbBytes=getSuperblockBytes(header);

    int inFd = open(inputPath.c_str(), O_RDONLY);
    if (inFd < 0) {
        std::cerr << "Error: Cannot open input file: " << inputPath << std::endl;
        return false;
    }

    std::ofstream outFile(outputPath, std::ios::binary);
    if (!outFile) {
        close(inFd);
        std::cerr << "Error: Cannot create output file: " << outputPath << std::endl;
        return false;
    }

    std::vector<char> outBuf(16 * 1024 * 1024);
    outFile.rdbuf()->pubsetbuf(outBuf.data(), outBuf.size());
    outFile.write(reinterpret_cast<const char*>(&header), sizeof(header));

    std::vector<uint64_t> sbOffsets(totalSB);
    for (uint64_t sz=0; sz<sgZ; ++sz)
        for (uint64_t sy=0; sy<sgY; ++sy)
            for (uint64_t sx=0; sx<sgX; ++sx) {
                uint64_t idx = (sz*sgY+sy)*sgX+sx;
                sbOffsets[idx] = sizeof(header) + idx * sbBytes;
            }

    std::vector<CompressedBlockIndex> compIndex;
    std::vector<uint8_t> leafBuf;
    std::vector<uint8_t> compBuf;
    if (compress) {
#ifdef ERWT3D_HAVE_LZ4
        compIndex.resize(totalSB);
        leafBuf.resize(sbBytes);
        compBuf.resize(LZ4_compressBound(static_cast<int>(sbBytes)));
        std::cout << "Compression: lz4, sb_bytes=" << sbBytes << std::endl;
#else
        std::cerr << "Warning: lz4 not available, disabling compression" << std::endl;
        compress = false;
#endif
    }

    bool doPanels = (panelAxis == 0) && panelStride > 0 && panelStride <= superX;
    uint64_t planeBytes = superY * superZ * sizeof(float);
    uint64_t panelCount = doPanels ? (superX + panelStride - 1) / panelStride : 0;
    uint64_t sbPanelBytes = doPanels ? panelCount * planeBytes : 0;
    uint64_t panelIndexBytes = doPanels ? totalSB * sizeof(uint64_t) : 0;

    uint64_t mainDataEnd = sizeof(header) + totalSB * sbBytes;
    uint64_t panelIndexOff = doPanels ? mainDataEnd : 0;
    uint64_t panelDataStart = doPanels ? panelIndexOff + panelIndexBytes : 0;

    std::vector<uint64_t> panelIndex;
    if (doPanels) {
        panelIndex.resize(totalSB);
        outFile.seekp(panelIndexOff);
        std::vector<char> placeholder(panelIndexBytes, 0);
        outFile.write(placeholder.data(), panelIndexBytes);
    }

    const uint64_t yzFloats = ny * nz;
    const uint64_t yzBytes = yzFloats * sizeof(float);
    const uint64_t slabX = superX;

    size_t budgetBytes = memoryLimitMB * 1024ULL * 1024ULL;
    const uint64_t requiredBytes = slabX * yzBytes + sbBytes + compBuf.size() + 64 * 1024 * 1024;
    if (budgetBytes < requiredBytes) {
        std::cerr << "Error: memory limit " << memoryLimitMB << " MB too small. "
                  << "Required at least " << (requiredBytes / (1024 * 1024) + 1)
                  << " MB for one X-slab (" << slabX << " planes) + superblock + compression buffer."
                  << std::endl;
        close(inFd);
        return false;
    }
    uint64_t usableSlabX = std::min<uint64_t>(slabX, nx);

    std::vector<float> slab(usableSlabX * yzFloats);
    std::vector<float> sb(superX * superY * superZ);
    std::vector<float> planeBuf(doPanels ? superY * superZ : 0);

    std::cout << "Sequential conversion: x-slab=" << usableSlabX
              << ", sgX=" << sgX << ", sgY=" << sgY << ", sgZ=" << sgZ << std::endl;
    if (doPanels) {
        std::cout << "X-panels: stride=" << panelStride << ", " << panelCount << " planes/sb" << std::endl;
    }

    auto startTime = std::chrono::high_resolution_clock::now();
    uint64_t panelWritePos = panelDataStart;
    uint64_t slabsProcessed = 0;
    uint64_t totalSlabs = (nx + usableSlabX - 1) / usableSlabX;

    for (uint64_t xStart = 0; xStart < nx; xStart += usableSlabX) {
        uint64_t currentX = std::min(usableSlabX, nx - xStart);
        uint64_t slabFloats = currentX * yzFloats;
        uint64_t slabReadBytes = slabFloats * sizeof(float);

        ssize_t n = pread(inFd, slab.data(), slabReadBytes,
                          xStart * yzBytes);
        if (n != static_cast<ssize_t>(slabReadBytes)) {
            std::cerr << "Error reading X-slab at x=" << xStart << std::endl;
            close(inFd);
            return false;
        }

        // For every superblock whose X range intersects this slab, pack it.
        uint64_t sxStart = xStart / superX;
        uint64_t sxEnd = std::min((xStart + currentX + superX - 1) / superX, sgX);

        for (uint64_t sx = sxStart; sx < sxEnd; ++sx) {
            uint64_t sbStartX = sx * superX;
            uint64_t localXStart = (sbStartX >= xStart) ? (sbStartX - xStart) : 0;
            uint64_t localXEnd = std::min(xStart + currentX - sbStartX, static_cast<uint64_t>(superX));

            for (uint64_t sy = 0; sy < sgY; ++sy) {
                for (uint64_t sz = 0; sz < sgZ; ++sz) {
                    std::memset(sb.data(), 0, sb.size() * sizeof(float));

                    uint64_t startY = sy * superY;
                    uint64_t startZ = sz * superZ;

                    for (uint64_t lz = 0; lz < superZ; ++lz) {
                        uint64_t gz = startZ + lz; if (gz >= nz) break;
                        for (uint64_t ly = 0; ly < superY; ++ly) {
                            uint64_t gy = startY + ly; if (gy >= ny) break;
                            for (uint64_t lx = localXStart; lx < localXEnd; ++lx) {
                                uint64_t gx = sbStartX + lx;
                                // In the slab, offset of (gx, gy, gz) relative to xStart.
                                uint64_t slabOff = (lx * ny + gy) * nz + gz;
                                sb[(lz * superY + ly) * superX + lx] = slab[slabOff];
                            }
                        }
                    }

                    uint64_t idx = (sz * sgY + sy) * sgX + sx;

                    if (compress) {
                        writeLeavesToBuffer(leafBuf.data(), header, sb);
                        CompressedBlockIndex entry;
#ifdef ERWT3D_HAVE_LZ4
                        int compSize = LZ4_compress_default(
                            reinterpret_cast<const char*>(leafBuf.data()),
                            reinterpret_cast<char*>(compBuf.data()),
                            static_cast<int>(sbBytes),
                            static_cast<int>(compBuf.size()));
                        if (compSize > 0 && static_cast<uint64_t>(compSize) < sbBytes * 95 / 100) {
                            entry.file_offset = static_cast<uint64_t>(outFile.tellp());
                            entry.compressed_size = static_cast<uint64_t>(compSize);
                            entry.is_compressed = 1;
                            std::memset(entry.padding, 0, sizeof(entry.padding));
                            outFile.write(reinterpret_cast<const char*>(compBuf.data()), compSize);
                        } else
#endif
                        {
                            entry.file_offset = static_cast<uint64_t>(outFile.tellp());
                            entry.compressed_size = static_cast<uint32_t>(sbBytes);
                            entry.is_compressed = 0;
                            std::memset(entry.padding, 0, sizeof(entry.padding));
                            outFile.write(reinterpret_cast<const char*>(leafBuf.data()), sbBytes);
                        }
                        compIndex[idx] = entry;
                    } else {
                        outFile.seekp(sbOffsets[idx]);
                        writeLeaves(outFile, header, sb);
                    }

                    if (doPanels) {
                        panelIndex[idx] = panelWritePos;
                        outFile.seekp(panelWritePos);
                        for (uint32_t lx = 0; lx < superX; lx += panelStride) {
                            for (uint64_t z = 0; z < superZ; ++z)
                                for (uint64_t y = 0; y < superY; ++y)
                                    planeBuf[z * superY + y] = sb[(z * superY + y) * superX + lx];
                            outFile.write(reinterpret_cast<const char*>(planeBuf.data()), planeBytes);
                        }
                        panelWritePos += sbPanelBytes;
                    }
                }
            }
        }

        ++slabsProcessed;
        if (slabsProcessed % 10 == 0 || slabsProcessed == totalSlabs) {
            auto now = std::chrono::high_resolution_clock::now();
            double elapsed = std::chrono::duration<double>(now - startTime).count();
            double progress = 100.0 * slabsProcessed / totalSlabs;
            double speed = elapsed > 0 ? slabsProcessed / elapsed : 0.0;
            double eta = speed > 0 ? (totalSlabs - slabsProcessed) / speed : 0.0;
            std::cout << "\rProgress: " << std::fixed << std::setprecision(1) << progress << "% "
                      << "(" << slabsProcessed << "/" << totalSlabs << " slabs) "
                      << std::setprecision(1) << speed << " slabs/s "
                      << "ETA: " << std::setprecision(0) << eta << "s" << std::flush;
        }
    }

    close(inFd);
    std::cout << std::endl;

    if (doPanels) {
        outFile.seekp(panelIndexOff);
        outFile.write(reinterpret_cast<const char*>(panelIndex.data()), panelIndexBytes);

        outFile.seekp(0);
        header.flags |= FLAG_HAS_X_PANELS;
        header.reserved[0] = panelStride;
        header.reserved[3] = panelDataStart;
        header.reserved[4] = panelIndexOff;
        header.reserved[5] = panelWritePos - panelDataStart;
        outFile.write(reinterpret_cast<const char*>(&header), sizeof(header));

        std::cout << "Panels written: " << (panelWritePos - panelDataStart) / (1024*1024) << " MB" << std::endl;
    }

    if (compress && !compIndex.empty()) {
        uint64_t indexOffset = static_cast<uint64_t>(outFile.tellp());
        uint64_t indexBytes = compIndex.size() * sizeof(CompressedBlockIndex);
        outFile.write(reinterpret_cast<const char*>(compIndex.data()), indexBytes);

        uint64_t totalCompBytes = 0;
        uint64_t compCount = 0;
        for (const auto& e : compIndex) {
            totalCompBytes += e.compressed_size;
            if (e.is_compressed) ++compCount;
        }

        outFile.seekp(0);
        header.flags |= FLAG_COMPRESSED;
        header.reserved[19] = indexOffset;
        header.reserved[20] = compIndex.size();
        outFile.write(reinterpret_cast<const char*>(&header), sizeof(header));

        std::cout << "Compression: " << compCount << "/" << compIndex.size()
                  << " blocks compressed, index at " << indexOffset
                  << " (" << indexBytes << " bytes)" << std::endl;
    }

    std::cout << "Conversion complete." << std::endl;
    return true;
}

bool writeERWT3D(const std::string& outputPath,
                 const float* rawData,
                 uint64_t nx, uint64_t ny, uint64_t nz,
                 uint32_t superX, uint32_t superY, uint32_t superZ,
                 uint32_t leafX, uint32_t leafY, uint32_t leafZ,
                 int, size_t,
                 uint32_t panelAxis, uint32_t panelStride) {
    bool doPanels = (panelAxis == 0) && panelStride > 0 && panelStride <= superX;

    ERWT3DHeader header;
    initHeader(header);
    header.nx=nx; header.ny=ny; header.nz=nz;
    header.super_x=superX; header.super_y=superY; header.super_z=superZ;
    header.leaf_x=leafX; header.leaf_y=leafY; header.leaf_z=leafZ;

    uint64_t sgX=getSuperGridX(header), sgY=getSuperGridY(header), sgZ=getSuperGridZ(header);
    uint64_t totalSB=sgX*sgY*sgZ, sbBytes=getSuperblockBytes(header);
    uint64_t rawSize=getRawSize(header);
    uint64_t planeBytes=superY*superZ*sizeof(float);
    uint64_t panelCount=doPanels?(superX+panelStride-1)/panelStride:0;
    uint64_t sbPanelBytes=panelCount*planeBytes;
    uint64_t panelDataSize=totalSB*sbPanelBytes;
    uint64_t panelIndexBytes=totalSB*sizeof(uint64_t);
    uint64_t mainSize=sizeof(header)+totalSB*sbBytes;
    uint64_t projectedSize=mainSize+(doPanels?panelDataSize+panelIndexBytes:0);
    double projectedRatio=static_cast<double>(projectedSize)/rawSize;

    if (doPanels && projectedRatio > 1.45) {
        std::cerr << "Error: projected storage ratio " << projectedRatio
                  << "x exceeds 1.45x limit (main=" << mainSize << " panel=" << panelDataSize+panelIndexBytes << ")"
                  << std::endl;
        return false;
    }
    if (doPanels) {
        std::cout << "Projected storage ratio: " << projectedRatio
                  << "x (main=" << mainSize << " panel=" << panelDataSize+panelIndexBytes << ")" << std::endl;
    }

    std::ofstream file(outputPath, std::ios::binary);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));

    std::vector<float> sb(superX*superY*superZ);

    // Pass 1: write superblocks (in-memory data, streaming)
    for (uint64_t sz=0; sz<sgZ; ++sz)
        for (uint64_t sy=0; sy<sgY; ++sy)
            for (uint64_t sx=0; sx<sgX; ++sx) {
                fillSuperBuffer(sb, rawData, nx, ny, nz, sx, sy, sz, superX, superY, superZ);
                writeLeaves(file, header, sb);
            }

    // Pass 2: generate panels from in-memory raw data
    if (doPanels) {
        uint64_t panelIndexOff=static_cast<uint64_t>(file.tellp());
        std::vector<uint64_t> panelIndex(totalSB);
        file.seekp(panelIndexOff+panelIndexBytes); // skip index for now
        std::vector<float> plane(superY*superZ);
        for (uint64_t sz=0; sz<sgZ; ++sz)
            for (uint64_t sy=0; sy<sgY; ++sy)
                for (uint64_t sx=0; sx<sgX; ++sx) {
                    uint64_t si=(sz*sgY+sy)*sgX+sx;
                    panelIndex[si]=static_cast<uint64_t>(file.tellp());
                    // Re-fill superbuffer (no snapshot!)
                    fillSuperBuffer(sb, rawData, nx, ny, nz, sx, sy, sz, superX, superY, superZ);
                    for (uint32_t lx=0; lx<superX; lx+=panelStride) {
                        for (uint64_t z=0; z<superZ; ++z)
                            for (uint64_t y=0; y<superY; ++y)
                                plane[z*superY+y]=sb[(z*superY+y)*superX+lx];
                        file.write(reinterpret_cast<const char*>(plane.data()), planeBytes);
                    }
                }
        uint64_t panelDataStart=panelIndexOff+panelIndexBytes;
        uint64_t panelEnd=static_cast<uint64_t>(file.tellp());
        file.seekp(panelIndexOff);
        file.write(reinterpret_cast<const char*>(panelIndex.data()), panelIndexBytes);
        file.seekp(0);
        header.flags|=FLAG_HAS_X_PANELS;
        header.reserved[0]=panelStride;
        header.reserved[3]=panelDataStart;
        header.reserved[4]=panelIndexOff;
        header.reserved[5]=panelEnd-panelDataStart;
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    }
    return true;
}

bool writeERWT3DFromFile(const std::string& outputPath,
                         const std::string& inputPath,
                         uint64_t nx, uint64_t ny, uint64_t nz,
                         uint32_t superX, uint32_t superY, uint32_t superZ,
                         uint32_t leafX, uint32_t leafY, uint32_t leafZ,
                         int numThreads, size_t memoryLimitMB,
                         uint32_t panelAxis, uint32_t panelStride,
                         bool compress) {
    std::cout << "Using sequential read strategy (HDD optimized)" << std::endl;
    if (compress) std::cout << "Compression: enabled (lz4)" << std::endl;
    return writeERWT3DFromFileSequential(outputPath, inputPath,
                                          nx, ny, nz,
                                          superX, superY, superZ,
                                          leafX, leafY, leafZ,
                                          numThreads, memoryLimitMB,
                                          panelAxis, panelStride, compress);
}

} // namespace erwt3d
