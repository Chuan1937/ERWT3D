#include "erwt3d/writer.hpp"
#include "erwt3d/morton.hpp"
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
                sb[(z*superY+y)*superX+x] = rawData[(gz*ny+gy)*nx+gx];
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
            uint64_t foff = ((gz*ny+gy)*nx+startX)*sizeof(float);
            uint64_t vx = std::min(static_cast<uint64_t>(superX), nx-startX);
            inFile.seekg(foff); inFile.clear();
            std::vector<float> row(vx);
            inFile.read(reinterpret_cast<char*>(row.data()), vx*sizeof(float));
            for (uint64_t x=0; x<vx; ++x) sb[(z*superY+y)*superX+x] = row[x];
        }
    }
}

// mmap版本: 直接从内存映射读取，避免大量seek
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
            const float* src = rawData + (gz*ny+gy)*nx+startX;
            for (uint64_t x=0; x<vx; ++x) sb[(z*superY+y)*superX+x] = src[x];
        }
    }
}

// 顺序读取策略: 按行顺序读取输入文件，分配到superblock缓冲区
// 适用于HDD，避免随机访问
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

    // 打开输入文件
    std::ifstream inFile(inputPath, std::ios::binary);
    if (!inFile) {
        std::cerr << "Error: Cannot open input file: " << inputPath << std::endl;
        return false;
    }

    // 创建输出文件
    std::ofstream outFile(outputPath, std::ios::binary);
    if (!outFile) {
        std::cerr << "Error: Cannot create output file: " << outputPath << std::endl;
        return false;
    }

    // 增大输出缓冲区到16MB
    std::vector<char> outBuf(16 * 1024 * 1024);
    outFile.rdbuf()->pubsetbuf(outBuf.data(), outBuf.size());

    // 写入header
    outFile.write(reinterpret_cast<const char*>(&header), sizeof(header));

    // 计算每个superblock在输出文件中的偏移
    std::vector<uint64_t> sbOffsets(totalSB);
    for (uint64_t sz=0; sz<sgZ; ++sz)
        for (uint64_t sy=0; sy<sgY; ++sy)
            for (uint64_t sx=0; sx<sgX; ++sx) {
                uint64_t idx = (sz*sgY+sy)*sgX+sx;
                sbOffsets[idx] = sizeof(header) + idx * sbBytes;
            }

    // Compression support
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

    // Panel support
    bool doPanels = (panelAxis == 0) && panelStride > 0 && panelStride <= superX;
    uint64_t planeBytes = superY * superZ * sizeof(float);
    uint64_t panelCount = doPanels ? superX / panelStride : 0;
    uint64_t sbPanelBytes = doPanels ? panelCount * planeBytes : 0;
    uint64_t panelIndexBytes = doPanels ? totalSB * sizeof(uint64_t) : 0;

    // Reserve space for panel index after main data
    uint64_t mainDataEnd = sizeof(header) + totalSB * sbBytes;
    uint64_t panelIndexOff = doPanels ? mainDataEnd : 0;
    uint64_t panelDataStart = doPanels ? panelIndexOff + panelIndexBytes : 0;

    std::vector<uint64_t> panelIndex;
    if (doPanels) {
        panelIndex.resize(totalSB);
        // Pre-allocate panel file space: write placeholder index
        outFile.seekp(panelIndexOff);
        std::vector<char> placeholder(panelIndexBytes, 0);
        outFile.write(placeholder.data(), panelIndexBytes);
    }

    // 分批处理：每次处理一批sy值，减少seek次数
    // 同一sy范围内的行是连续的，可以顺序读取
    uint64_t maxBatchSY = std::max(uint64_t(1), uint64_t(memoryLimitMB * 1024ULL * 1024ULL / (sgX * sgZ * sbBytes)));
    maxBatchSY = std::min(maxBatchSY, sgY);

    std::cout << "Sequential conversion: batch_sy=" << maxBatchSY << ", sgY=" << sgY << std::endl;
    if (doPanels) {
        std::cout << "X-panels: stride=" << panelStride << ", " << panelCount << " planes/sb" << std::endl;
    }

    // 增大读取缓冲区（一次读取多行）
    uint64_t rowsPerBatch = std::min(uint64_t(1024), ny);
    std::vector<float> rowBuf(nx * rowsPerBatch);
    std::vector<float> planeBuf(doPanels ? superY * superZ : 0);

    auto startTime = std::chrono::high_resolution_clock::now();
    uint64_t totalRowsProcessed = 0;
    uint64_t totalRows = nz * ny;
    uint64_t panelWritePos = panelDataStart;  // current panel write position

    for (uint64_t syStart=0; syStart<sgY; syStart+=maxBatchSY) {
        uint64_t syEnd = std::min(syStart + maxBatchSY, sgY);
        uint64_t batchSYCount = syEnd - syStart;

        // 分配当前批次的superblock缓冲区
        std::vector<std::vector<float>> sbBuffers(sgX * batchSYCount * sgZ);
        for (auto& buf : sbBuffers) buf.resize(superX * superY * superZ, 0.0f);

        // 顺序读取输入文件，分配到superblock缓冲区
        for (uint64_t z=0; z<nz; ++z) {
            uint64_t sz = z / superZ;
            uint64_t localZ = z % superZ;

            // 计算当前sy范围的行范围
            uint64_t yStart = syStart * superY;
            uint64_t yEnd = std::min(syEnd * superY, ny);

            // 批量读取多行数据
            for (uint64_t yBatchStart=yStart; yBatchStart<yEnd; yBatchStart+=rowsPerBatch) {
                uint64_t yBatchEnd = std::min(yBatchStart + rowsPerBatch, yEnd);
                uint64_t rowsToRead = yBatchEnd - yBatchStart;

                // 读取一批行
                uint64_t batchOffset = (z * ny + yBatchStart) * nx * sizeof(float);
                inFile.seekg(batchOffset);
                inFile.read(reinterpret_cast<char*>(rowBuf.data()), rowsToRead * nx * sizeof(float));
                if (!inFile) {
                    std::cerr << "Error reading rows at z=" << z << ", y=" << yBatchStart << std::endl;
                    return false;
                }

                // 分配到各个superblock
                for (uint64_t y=yBatchStart; y<yBatchEnd; ++y) {
                    uint64_t sy = y / superY;
                    uint64_t localY = y % superY;
                    uint64_t syIdx = sy - syStart;
                    uint64_t rowIdx = y - yBatchStart;

                    for (uint64_t sx=0; sx<sgX; ++sx) {
                        uint64_t startX = sx * superX;
                        uint64_t vx = std::min(uint64_t(superX), nx - startX);
                        uint64_t localX = 0;

                        uint64_t sbIdx = (sx * batchSYCount + syIdx) * sgZ + sz;
                        auto& sbBuf = sbBuffers[sbIdx];
                        uint64_t sbOffset = (localZ * superY + localY) * superX;

                        for (uint64_t x=startX; x<startX+vx; ++x) {
                            sbBuf[sbOffset + localX] = rowBuf[rowIdx * nx + x];
                            localX++;
                        }
                    }

                    totalRowsProcessed++;
                }
            }

            // 输出进度
            if (z % 100 == 0 || z == nz-1) {
                auto now = std::chrono::high_resolution_clock::now();
                double elapsed = std::chrono::duration<double>(now - startTime).count();
                double progress = 100.0 * totalRowsProcessed / totalRows;
                double speed = totalRowsProcessed / elapsed;
                double eta = (totalRows - totalRowsProcessed) / speed;
                std::cout << "\rProgress: " << std::fixed << std::setprecision(1) << progress << "% "
                          << "(" << totalRowsProcessed << "/" << totalRows << " rows) "
                          << std::setprecision(1) << speed << " rows/s "
                          << "ETA: " << std::setprecision(0) << eta << "s" << std::flush;
            }
        }

        // 写入当前批次的superblocks + panel数据
        for (uint64_t sy=syStart; sy<syEnd; ++sy) {
            uint64_t syIdx = sy - syStart;
            for (uint64_t sz=0; sz<sgZ; ++sz) {
                for (uint64_t sx=0; sx<sgX; ++sx) {
                    uint64_t idx = (sz*sgY+sy)*sgX+sx;
                    uint64_t sbIdx = (sx * batchSYCount + syIdx) * sgZ + sz;

                    if (compress) {
                        writeLeavesToBuffer(leafBuf.data(), header, sbBuffers[sbIdx]);
                        CompressedBlockIndex entry;
#ifdef ERWT3D_HAVE_LZ4
                        int compSize = LZ4_compress_default(
                            reinterpret_cast<const char*>(leafBuf.data()),
                            reinterpret_cast<char*>(compBuf.data()),
                            static_cast<int>(sbBytes),
                            static_cast<int>(compBuf.size()));
                        if (compSize > 0 && static_cast<uint64_t>(compSize) < sbBytes * 95 / 100) {
                            entry.file_offset = static_cast<uint64_t>(outFile.tellp());
                            entry.compressed_size = static_cast<uint32_t>(compSize);
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
                        uint64_t offset = sbOffsets[idx];
                        outFile.seekp(offset);
                        writeLeaves(outFile, header, sbBuffers[sbIdx]);
                    }

                    // Write panel data for this superblock
                    if (doPanels) {
                        panelIndex[idx] = panelWritePos;
                        outFile.seekp(panelWritePos);
                        const auto& sbBuf = sbBuffers[sbIdx];
                        for (uint32_t lx = 0; lx < superX; lx += panelStride) {
                            for (uint64_t z = 0; z < superZ; ++z)
                                for (uint64_t y = 0; y < superY; ++y)
                                    planeBuf[z * superY + y] = sbBuf[(z * superY + y) * superX + lx];
                            outFile.write(reinterpret_cast<const char*>(planeBuf.data()), planeBytes);
                        }
                        panelWritePos += sbPanelBytes;
                    }
                }
            }
        }
    }

    std::cout << std::endl;

    // Write panel index and update header
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

    // Write compression index and update header
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
    uint64_t panelCount=doPanels?superX/panelStride:0;
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
