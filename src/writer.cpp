#include "erwt3d/writer.hpp"
#include "erwt3d/morton.hpp"
#include "erwt3d/thread_pool.hpp"
#include <fstream>
#include <vector>
#include <cstring>
#include <algorithm>

namespace erwt3d {

bool writeERWT3D(const std::string& outputPath,
                 const float* rawData,
                 uint64_t nx, uint64_t ny, uint64_t nz,
                 uint32_t superX, uint32_t superY, uint32_t superZ,
                 uint32_t leafX, uint32_t leafY, uint32_t leafZ,
                 int numThreads, size_t memoryLimitMB) {
    // Initialize header
    ERWT3DHeader header;
    initHeader(header);
    header.nx = nx;
    header.ny = ny;
    header.nz = nz;
    header.super_x = superX;
    header.super_y = superY;
    header.super_z = superZ;
    header.leaf_x = leafX;
    header.leaf_y = leafY;
    header.leaf_z = leafZ;
    
    // Calculate dimensions
    uint64_t superGridX = getSuperGridX(header);
    uint64_t superGridY = getSuperGridY(header);
    uint64_t superGridZ = getSuperGridZ(header);
    uint64_t superBytes = getSuperblockBytes(header);
    uint64_t leafBytes = getLeafBytes(header);
    uint64_t totalSuperblocks = superGridX * superGridY * superGridZ;
    
    // Open output file
    std::ofstream file(outputPath, std::ios::binary);
    if (!file) {
        return false;
    }
    
    // Write header
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    
    // Allocate buffer for one superblock
    std::vector<float> superBuffer(superX * superY * superZ);
    std::vector<float> leafBuffer(leafX * leafY * leafZ);
    
    // Process superblocks in Z-Y-X order (sequential file layout)
    for (uint64_t sz = 0; sz < superGridZ; ++sz) {
        for (uint64_t sy = 0; sy < superGridY; ++sy) {
            for (uint64_t sx = 0; sx < superGridX; ++sx) {
                // Fill superbuffer from raw data
                std::memset(superBuffer.data(), 0, superBytes);
                
                uint64_t startX = sx * superX;
                uint64_t startY = sy * superY;
                uint64_t startZ = sz * superZ;
                
                for (uint64_t z = 0; z < superZ; ++z) {
                    uint64_t globalZ = startZ + z;
                    if (globalZ >= nz) break;
                    
                    for (uint64_t y = 0; y < superY; ++y) {
                        uint64_t globalY = startY + y;
                        if (globalY >= ny) break;
                        
                        for (uint64_t x = 0; x < superX; ++x) {
                            uint64_t globalX = startX + x;
                            if (globalX >= nx) break;
                            
                            uint64_t srcIdx = (globalZ * ny + globalY) * nx + globalX;
                            uint64_t dstIdx = (z * superY + y) * superX + x;
                            superBuffer[dstIdx] = rawData[srcIdx];
                        }
                    }
                }
                
                // Write leaf blocks in Morton order
                uint64_t totalLeafs = getTotalLeafsPerSuper(header);
                for (uint64_t j = 0; j < totalLeafs; ++j) {
                    uint32_t lx, ly, lz;
                    unmorton3D(j, lx, ly, lz);
                    
                    // Skip invalid leaf blocks
                    if (lx >= getLeafsPerSuperX(header) || 
                        ly >= getLeafsPerSuperY(header) || 
                        lz >= getLeafsPerSuperZ(header)) continue;
                    
                    uint64_t srcX = lx * leafX;
                    uint64_t srcY = ly * leafY;
                    uint64_t srcZ = lz * leafZ;
                    
                    for (uint64_t z = 0; z < leafZ; ++z) {
                        for (uint64_t y = 0; y < leafY; ++y) {
                            for (uint64_t x = 0; x < leafX; ++x) {
                                uint64_t srcIdx = ((srcZ + z) * superY + (srcY + y)) * superX + (srcX + x);
                                uint64_t dstIdx = (z * leafY + y) * leafX + x;
                                leafBuffer[dstIdx] = superBuffer[srcIdx];
                            }
                        }
                    }
                    
                    file.write(reinterpret_cast<const char*>(leafBuffer.data()), leafBytes);
                }
            }
        }
    }
    
    return true;
}

bool writeERWT3DFromFile(const std::string& outputPath,
                         const std::string& inputPath,
                         uint64_t nx, uint64_t ny, uint64_t nz,
                         uint32_t superX, uint32_t superY, uint32_t superZ,
                         uint32_t leafX, uint32_t leafY, uint32_t leafZ,
                         int numThreads, size_t memoryLimitMB) {
    // Initialize header
    ERWT3DHeader header;
    initHeader(header);
    header.nx = nx;
    header.ny = ny;
    header.nz = nz;
    header.super_x = superX;
    header.super_y = superY;
    header.super_z = superZ;
    header.leaf_x = leafX;
    header.leaf_y = leafY;
    header.leaf_z = leafZ;
    
    uint64_t superGridX = getSuperGridX(header);
    uint64_t superGridY = getSuperGridY(header);
    uint64_t superGridZ = getSuperGridZ(header);
    uint64_t superBytes = getSuperblockBytes(header);
    uint64_t leafBytes = getLeafBytes(header);
    
    // Open input file
    std::ifstream inFile(inputPath, std::ios::binary);
    if (!inFile) return false;
    
    // Open output file
    std::ofstream outFile(outputPath, std::ios::binary);
    if (!outFile) return false;
    
    // Write header
    outFile.write(reinterpret_cast<const char*>(&header), sizeof(header));
    
    // Allocate buffer for one superblock
    std::vector<float> superBuffer(superX * superY * superZ);
    std::vector<float> leafBuffer(leafX * leafY * leafZ);
    
    // Process superblocks in Z-Y-X order
    for (uint64_t sz = 0; sz < superGridZ; ++sz) {
        for (uint64_t sy = 0; sy < superGridY; ++sy) {
            for (uint64_t sx = 0; sx < superGridX; ++sx) {
                std::memset(superBuffer.data(), 0, superBytes);
                
                uint64_t startX = sx * superX;
                uint64_t startY = sy * superY;
                uint64_t startZ = sz * superZ;
                
                // Read valid rows from raw file
                for (uint64_t z = 0; z < superZ; ++z) {
                    uint64_t globalZ = startZ + z;
                    if (globalZ >= nz) break;
                    
                    for (uint64_t y = 0; y < superY; ++y) {
                        uint64_t globalY = startY + y;
                        if (globalY >= ny) break;
                        
                        uint64_t fileOffset = ((globalZ * ny + globalY) * nx + startX) * sizeof(float);
                        uint64_t validX = std::min(static_cast<uint64_t>(superX), nx - startX);
                        
                        inFile.seekg(fileOffset);
                        inFile.clear();
                        std::vector<float> row(validX);
                        inFile.read(reinterpret_cast<char*>(row.data()), validX * sizeof(float));
                        
                        for (uint64_t x = 0; x < validX; ++x) {
                            uint64_t dstIdx = (z * superY + y) * superX + x;
                            superBuffer[dstIdx] = row[x];
                        }
                    }
                }
                
                // Write leaf blocks in Morton order
                uint64_t totalLeafs = getTotalLeafsPerSuper(header);
                for (uint64_t j = 0; j < totalLeafs; ++j) {
                    uint32_t lx, ly, lz;
                    unmorton3D(j, lx, ly, lz);
                    
                    if (lx >= getLeafsPerSuperX(header) || 
                        ly >= getLeafsPerSuperY(header) || 
                        lz >= getLeafsPerSuperZ(header)) continue;
                    
                    uint64_t srcX = lx * leafX;
                    uint64_t srcY = ly * leafY;
                    uint64_t srcZ = lz * leafZ;
                    
                    for (uint64_t z = 0; z < leafZ; ++z) {
                        for (uint64_t y = 0; y < leafY; ++y) {
                            for (uint64_t x = 0; x < leafX; ++x) {
                                uint64_t srcIdx = ((srcZ + z) * superY + (srcY + y)) * superX + (srcX + x);
                                uint64_t dstIdx = (z * leafY + y) * leafX + x;
                                leafBuffer[dstIdx] = superBuffer[srcIdx];
                            }
                        }
                    }
                    
                    outFile.write(reinterpret_cast<const char*>(leafBuffer.data()), leafBytes);
                }
            }
        }
    }
    
    return true;
}

} // namespace erwt3d