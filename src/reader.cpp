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
    fd_ = open(path.c_str(), O_RDONLY);
    if (fd_ < 0) return;
    
    if (read(fd_, &header_, sizeof(header_)) != sizeof(header_)) {
        close(fd_);
        fd_ = -1;
        return;
    }
    
    if (!validateHeader(header_)) {
        close(fd_);
        fd_ = -1;
        return;
    }
    
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
    
    // Bounds check
    switch (axis) {
        case SliceAxis::X: if (index >= header_.nx) return false; break;
        case SliceAxis::Y: if (index >= header_.ny) return false; break;
        case SliceAxis::Z: if (index >= header_.nz) return false; break;
    }
    
    SliceRequest request;
    request.axis = axis;
    request.index = index;
    
    SlicePlan plan = planSlice(header_, request);
    
    auto mergedExtents = mergeExtents(plan.extents);
    
    uint64_t totalReadSize = 0;
    for (const auto& ext : mergedExtents) {
        totalReadSize += ext.size;
    }
    
    std::vector<uint8_t> readBuffer(totalReadSize);
    
    uint64_t offset = 0;
    for (const auto& ext : mergedExtents) {
        ssize_t bytesRead = pread(fd_, readBuffer.data() + offset, ext.size, ext.offset);
        if (bytesRead != static_cast<ssize_t>(ext.size)) {
            return false;
        }
        offset += ext.size;
    }
    
    executeSlice(header_, plan, readBuffer.data(), output);
    
    return true;
}

bool ERWT3DReader::readLineX(uint64_t y, uint64_t z, float* output) {
    if (fd_ < 0) return false;
    
    const uint64_t nx = header_.nx;
    const uint64_t ny = header_.ny;
    const uint64_t nz = header_.nz;
    
    // Bounds check
    if (y >= ny || z >= nz) return false;
    
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
    std::vector<uint64_t> extentBaseX;
    
    for (uint64_t superX = 0; superX < getSuperGridX(header_); ++superX) {
        uint64_t superIdx = (superZ * getSuperGridY(header_) + superY) * getSuperGridX(header_) + superX;
        uint64_t superOffset = header_.data_offset + superIdx * superBytes;
        
        for (uint64_t lx2 = 0; lx2 < leafsPerSuperX; ++lx2) {
            uint64_t baseX = superX * sx + lx2 * lx;
            if (baseX >= nx) continue;
            
            uint64_t leafMorton = morton3D(lx2, leafY, leafZ);
            uint64_t leafOffset = superOffset + leafMorton * leafBytes;
            extents.emplace_back(leafOffset, leafBytes);
            extentBaseX.push_back(baseX);
        }
    }
    
    auto mergedExtents = mergeExtents(extents);
    
    uint64_t totalReadSize = 0;
    for (const auto& ext : mergedExtents) {
        totalReadSize += ext.size;
    }
    
    std::vector<uint8_t> readBuffer(totalReadSize);
    
    if (!readExtents(mergedExtents, readBuffer.data())) {
        return false;
    }
    
    const uint64_t srcLineOffset = (inLeafZ * ly + inLeafY) * lx;
    
    for (size_t i = 0; i < extents.size(); ++i) {
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
        uint64_t baseX = extentBaseX[i];
        uint64_t validLx = std::min(lx, nx - baseX);
        
        for (uint64_t dx = 0; dx < validLx; ++dx) {
            uint64_t globalX = baseX + dx;
            output[globalX] = leafData[srcLineOffset + dx];
        }
    }
    
    return true;
}

// readFull is intended for tests and small volumes only.
// For large volumes, use readFullToFile().
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
    
    // Enforce memory limit: superblock buffer must fit
    if (superBytes + leafBytes > memoryLimitMB * 1024 * 1024) {
        return false;
    }
    
    std::vector<uint8_t> superBuffer(superBytes);
    
    for (uint64_t szi = 0; szi < getSuperGridZ(header_); ++szi) {
        for (uint64_t syi = 0; syi < getSuperGridY(header_); ++syi) {
            for (uint64_t sxi = 0; sxi < getSuperGridX(header_); ++sxi) {
                uint64_t superIdx = (szi * getSuperGridY(header_) + syi) * getSuperGridX(header_) + sxi;
                uint64_t superOffset = header_.data_offset + superIdx * superBytes;
                
                ssize_t bytesRead = pread(fd_, superBuffer.data(), superBytes, superOffset);
                if (bytesRead != static_cast<ssize_t>(superBytes)) {
                    return false;
                }
                
                uint64_t startX = sxi * sx;
                uint64_t startY = syi * sy;
                uint64_t startZ = szi * sz;
                
                for (uint64_t lzi = 0; lzi < leafsPerSuperZ; ++lzi) {
                    for (uint64_t lyi = 0; lyi < leafsPerSuperY; ++lyi) {
                        for (uint64_t lxi = 0; lxi < leafsPerSuperX; ++lxi) {
                            uint64_t leafMorton = morton3D(lxi, lyi, lzi);
                            if (lxi >= leafsPerSuperX || lyi >= leafsPerSuperY || lzi >= leafsPerSuperZ) continue;
                            const float* leafData = reinterpret_cast<const float*>(
                                superBuffer.data() + leafMorton * leafBytes);
                            
                            uint64_t baseX = startX + lxi * lx;
                            uint64_t baseY = startY + lyi * ly;
                            uint64_t baseZ = startZ + lzi * lz;
                            
                            for (uint64_t z = 0; z < lz; ++z) {
                                uint64_t globalZ = baseZ + z;
                                if (globalZ >= nz) break;
                                for (uint64_t y = 0; y < ly; ++y) {
                                    uint64_t globalY = baseY + y;
                                    if (globalY >= ny) break;
                                    for (uint64_t x = 0; x < lx; ++x) {
                                        uint64_t globalX = baseX + x;
                                        if (globalX >= nx) break;
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
        }
    }
    
    return true;
}

bool ERWT3DReader::readFullToFile(const std::string& outputPath, int numThreads, size_t memoryLimitMB) {
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
    
    // Enforce memory limit
    if (superBytes + leafBytes > memoryLimitMB * 1024 * 1024) {
        return false;
    }
    
    int outFd = open(outputPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (outFd < 0) return false;
    
    uint64_t rawSize = nx * ny * nz * sizeof(float);
    if (ftruncate(outFd, rawSize) != 0) {
        close(outFd);
        return false;
    }
    
    std::vector<uint8_t> superBuffer(superBytes);
    
    for (uint64_t szi = 0; szi < getSuperGridZ(header_); ++szi) {
        for (uint64_t syi = 0; syi < getSuperGridY(header_); ++syi) {
            for (uint64_t sxi = 0; sxi < getSuperGridX(header_); ++sxi) {
                uint64_t superIdx = (szi * getSuperGridY(header_) + syi) * getSuperGridX(header_) + sxi;
                uint64_t superOffset = header_.data_offset + superIdx * superBytes;
                
                ssize_t bytesRead = pread(fd_, superBuffer.data(), superBytes, superOffset);
                if (bytesRead != static_cast<ssize_t>(superBytes)) {
                    close(outFd);
                    return false;
                }
                
                uint64_t startX = sxi * sx;
                uint64_t startY = syi * sy;
                uint64_t startZ = szi * sz;
                
                for (uint64_t lzi = 0; lzi < leafsPerSuperZ; ++lzi) {
                    for (uint64_t lyi = 0; lyi < leafsPerSuperY; ++lyi) {
                        for (uint64_t lxi = 0; lxi < leafsPerSuperX; ++lxi) {
                            uint64_t leafMorton = morton3D(lxi, lyi, lzi);
                            if (lxi >= leafsPerSuperX || lyi >= leafsPerSuperY || lzi >= leafsPerSuperZ) continue;
                            const float* leafData = reinterpret_cast<const float*>(
                                superBuffer.data() + leafMorton * leafBytes);
                            
                            uint64_t baseX = startX + lxi * lx;
                            uint64_t baseY = startY + lyi * ly;
                            uint64_t baseZ = startZ + lzi * lz;
                            
                            for (uint64_t z = 0; z < lz; ++z) {
                                uint64_t globalZ = baseZ + z;
                                if (globalZ >= nz) break;
                                for (uint64_t y = 0; y < ly; ++y) {
                                    uint64_t globalY = baseY + y;
                                    if (globalY >= ny) break;
                                    // Batch pwrite one contiguous x-segment per leaf row
                                    uint64_t globalX = baseX;
                                    if (globalX >= nx) break;
                                    uint64_t validLx = std::min(lx, nx - globalX);
                                    uint64_t srcIdx = (z * ly + y) * lx;
                                    uint64_t fileOffset = ((globalZ * ny + globalY) * nx + globalX) * sizeof(float);
                                    
                                    ssize_t written = pwrite(outFd, leafData + srcIdx, validLx * sizeof(float), fileOffset);
                                    if (written != static_cast<ssize_t>(validLx * sizeof(float))) {
                                        close(outFd);
                                        return false;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    
    if (close(outFd) != 0) return false;
    return true;
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