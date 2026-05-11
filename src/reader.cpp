#include "erwt3d/reader.hpp"
#include "erwt3d/morton.hpp"
#include "erwt3d/thread_pool.hpp"
#include <fstream>
#include <vector>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace erwt3d {

ERWT3DReader::ERWT3DReader(const std::string& path, size_t cacheMB) 
    : path_(path), fd_(-1) {
    // Open file
    fd_ = open(path.c_str(), O_RDONLY);
    if (fd_ < 0) {
        return;
    }
    
    // Read header
    if (read(fd_, &header_, sizeof(header_)) != sizeof(header_)) {
        close(fd_);
        fd_ = -1;
        return;
    }
    
    // Validate header
    if (!validateHeader(header_)) {
        close(fd_);
        fd_ = -1;
        return;
    }
    
    // Initialize cache
    if (cacheMB > 0) {
        cache_ = std::make_unique<LeafCache>(cacheMB * 1024 * 1024);
    }
}

ERWT3DReader::~ERWT3DReader() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

bool ERWT3DReader::readSlice(SliceAxis axis, uint64_t index, float* output) {
    if (fd_ < 0) return false;
    
    SliceRequest request;
    request.axis = axis;
    request.index = index;
    
    SlicePlan plan = planSlice(header_, request);
    
    // Merge extents
    auto mergedExtents = mergeExtents(plan.extents);
    
    // Calculate total read size
    uint64_t totalReadSize = 0;
    for (const auto& ext : mergedExtents) {
        totalReadSize += ext.size;
    }
    
    // Allocate read buffer
    std::vector<uint8_t> readBuffer(totalReadSize);
    
    // Read extents
    uint64_t offset = 0;
    for (const auto& ext : mergedExtents) {
        ssize_t bytesRead = pread(fd_, readBuffer.data() + offset, ext.size, ext.offset);
        if (bytesRead != static_cast<ssize_t>(ext.size)) {
            return false;
        }
        offset += ext.size;
    }
    
    // Execute slice plan
    executeSlice(header_, plan, readBuffer.data(), output);
    
    return true;
}

bool ERWT3DReader::readLineX(uint64_t y, uint64_t z, float* output) {
    if (fd_ < 0) return false;
    
    const uint64_t nx = header_.nx;
    const uint64_t sx = header_.super_x;
    const uint64_t sy = header_.super_y;
    const uint64_t sz = header_.super_z;
    const uint64_t lx = header_.leaf_x;
    const uint64_t ly = header_.leaf_y;
    const uint64_t lz = header_.leaf_z;
    const uint64_t superBytes = getSuperblockBytes(header_);
    const uint64_t leafBytes = getLeafBytes(header_);
    const uint64_t leafsPerSuperX = getLeafsPerSuperX(header_);
    
    uint64_t superY = y / sy;
    uint64_t superZ = z / sz;
    uint64_t localY = y % sy;
    uint64_t localZ = z % sz;
    uint64_t leafY = localY / ly;
    uint64_t leafZ = localZ / lz;
    uint64_t inLeafY = localY % ly;
    uint64_t inLeafZ = localZ % lz;
    
    std::vector<Extent> extents;
    
    for (uint64_t superX = 0; superX < getSuperGridX(header_); ++superX) {
        uint64_t superIdx = (superZ * getSuperGridY(header_) + superY) * getSuperGridX(header_) + superX;
        uint64_t superOffset = header_.data_offset + superIdx * superBytes;
        
        for (uint64_t lx2 = 0; lx2 < leafsPerSuperX; ++lx2) {
            uint64_t baseX = superX * sx + lx2 * lx;
            if (baseX >= nx) continue;
            
            uint64_t leafMorton = morton3D(lx2, leafY, leafZ);
            uint64_t leafOffset = superOffset + leafMorton * leafBytes;
            extents.emplace_back(leafOffset, leafBytes);
        }
    }
    
    // Merge extents
    auto mergedExtents = mergeExtents(extents);
    
    // Calculate total read size
    uint64_t totalReadSize = 0;
    for (const auto& ext : mergedExtents) {
        totalReadSize += ext.size;
    }
    
    // Allocate read buffer
    std::vector<uint8_t> readBuffer(totalReadSize);
    
    // Read extents
    if (!readExtents(mergedExtents, readBuffer.data())) {
        return false;
    }
    
    // Extract line data
    uint64_t outIdx = 0;
    
    for (size_t i = 0; i < extents.size(); ++i) {
        // Find merged extent containing this extent
        uint64_t srcOffset = 0;
        uint64_t bufferOffset = 0;
        for (size_t j = 0; j < mergedExtents.size(); ++j) {
            if (extents[i].offset >= mergedExtents[j].offset && 
                extents[i].offset < mergedExtents[j].end()) {
                srcOffset = bufferOffset + (extents[i].offset - mergedExtents[j].offset);
                break;
            }
            bufferOffset += mergedExtents[j].size;
        }
        
        const float* leafData = reinterpret_cast<const float*>(readBuffer.data() + srcOffset);
        
        // Extract one float from each leaf block
        uint64_t inLeafIdx = (inLeafZ * ly + inLeafY) * lx;
        output[outIdx++] = leafData[inLeafIdx];
    }
    
    return true;
}

bool ERWT3DReader::readFull(float* output, int numThreads, size_t memoryLimitMB) {
    if (fd_ < 0) return false;
    
    const uint64_t nx = header_.nx;
    const uint64_t ny = header_.ny;
    const uint64_t nz = header_.nz;
    const uint64_t sx = header_.super_x;
    const uint64_t sy = header_.super_y;
    const uint64_t sz = header_.super_z;
    const uint64_t lx = header_.leaf_x;
    const uint64_t ly = header_.leaf_y;
    const uint64_t lz = header_.leaf_z;
    const uint64_t superBytes = getSuperblockBytes(header_);
    const uint64_t leafBytes = getLeafBytes(header_);
    const uint64_t leafsPerSuperX = getLeafsPerSuperX(header_);
    const uint64_t leafsPerSuperY = getLeafsPerSuperY(header_);
    const uint64_t leafsPerSuperZ = getLeafsPerSuperZ(header_);
    
    // For simplicity, read entire file data region
    uint64_t dataSize = getTotalSuperblocks(header_) * superBytes;
    std::vector<uint8_t> readBuffer(dataSize);
    
    lseek(fd_, header_.data_offset, SEEK_SET);
    if (read(fd_, readBuffer.data(), dataSize) != static_cast<ssize_t>(dataSize)) {
        return false;
    }
    
    // Process superblocks in Z-Y-X order (matches writer layout)
    uint64_t superIdx = 0;
    for (uint64_t sz2 = 0; sz2 < getSuperGridZ(header_); ++sz2) {
        for (uint64_t sy2 = 0; sy2 < getSuperGridY(header_); ++sy2) {
            for (uint64_t sx2 = 0; sx2 < getSuperGridX(header_); ++sx2) {
                const uint8_t* superData = readBuffer.data() + superIdx * superBytes;
                ++superIdx;
                
                // Process leaf blocks in Morton order
                uint64_t totalLeafs = getTotalLeafsPerSuper(header_);
                for (uint64_t j = 0; j < totalLeafs; ++j) {
                    uint32_t lx2, ly2, lz2;
                    unmorton3D(j, lx2, ly2, lz2);
                    
                    if (lx2 >= leafsPerSuperX || ly2 >= leafsPerSuperY || lz2 >= leafsPerSuperZ) continue;
                    
                    const float* leafData = reinterpret_cast<const float*>(
                        superData + j * leafBytes);
                    
                    uint64_t baseX = sx2 * sx + lx2 * lx;
                    uint64_t baseY = sy2 * sy + ly2 * ly;
                    uint64_t baseZ = sz2 * sz + lz2 * lz;
                    
                    for (uint64_t z = 0; z < lz; ++z) {
                        uint64_t globalZ = baseZ + z;
                        if (globalZ >= nz) continue;
                        
                        for (uint64_t y = 0; y < ly; ++y) {
                            uint64_t globalY = baseY + y;
                            if (globalY >= ny) continue;
                            
                            for (uint64_t x = 0; x < lx; ++x) {
                                uint64_t globalX = baseX + x;
                                if (globalX >= nx) continue;
                                
                                uint64_t srcIdx = (z * ly + y) * lx + x;
                                uint64_t dstIdx = (globalZ * ny + globalY) * nx + globalX;
                                output[dstIdx] = leafData[srcIdx];
                            }
                        }
                    }
                }
            }
        }
    }
    
    return true;
}

bool ERWT3DReader::readFullToFile(const std::string& outputPath, int numThreads, size_t memoryLimitMB) {
    if (fd_ < 0) return false;
    
    uint64_t rawSize = getRawSize(header_);
    std::vector<float> rawData(rawSize / sizeof(float));
    
    if (!readFull(rawData.data(), numThreads, memoryLimitMB)) {
        return false;
    }
    
    std::ofstream outFile(outputPath, std::ios::binary);
    if (!outFile) {
        return false;
    }
    
    outFile.write(reinterpret_cast<const char*>(rawData.data()), rawSize);
    return outFile.good();
}

bool ERWT3DReader::readExtents(const std::vector<Extent>& extents, void* buffer) {
    uint8_t* buf = static_cast<uint8_t*>(buffer);
    uint64_t offset = 0;
    
    for (const auto& ext : extents) {
        ssize_t bytesRead = pread(fd_, buf + offset, ext.size, ext.offset);
        if (bytesRead != static_cast<ssize_t>(ext.size)) {
            return false;
        }
        offset += ext.size;
    }
    
    return true;
}

} // namespace erwt3d