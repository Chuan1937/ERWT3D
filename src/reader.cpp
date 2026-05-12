#include "erwt3d/reader.hpp"
#include "erwt3d/morton.hpp"
#include "erwt3d/thread_pool.hpp"
#include <algorithm>
#include <fstream>
#include <vector>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace erwt3d {

ERWT3DReader::ERWT3DReader(const std::string& path, size_t cacheMB) 
    : path_(path), fd_(-1), cacheMB_(cacheMB) {
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

void ERWT3DReader::setCacheMB(size_t cacheMB) {
    cacheMB_ = cacheMB;
    if (cacheMB > 0) {
        if (cache_) cache_->clear();
        cache_ = std::make_unique<LeafCache>(cacheMB * 1024 * 1024);
    } else {
        cache_.reset();
    }
}

bool ERWT3DReader::readOneExtent(uint64_t offset, uint64_t size, void* buffer) {
    if (!cache_) {
        ssize_t n = pread(fd_, buffer, size, offset);
        return n == static_cast<ssize_t>(size);
    }
    // Try cache first
    if (cache_->get(offset, buffer, size)) {
        return true;
    }
    ssize_t n = pread(fd_, buffer, size, offset);
    if (n != static_cast<ssize_t>(size)) return false;
    cache_->put(offset, buffer, size);
    return true;
}

// --- readSlice (legacy wrapper) ---
bool ERWT3DReader::readSlice(SliceAxis axis, uint64_t index, float* output) {
    return readSlice(axis, index, output, 1, 2048);
}

// --- readSlice with threading and memory control ---
bool ERWT3DReader::readSlice(SliceAxis axis, uint64_t index, float* output,
                              int numThreads, size_t memoryLimitMB) {
    if (fd_ < 0) return false;
    
    switch (axis) {
        case SliceAxis::X: if (index >= header_.nx) return false; break;
        case SliceAxis::Y: if (index >= header_.ny) return false; break;
        case SliceAxis::Z: if (index >= header_.nz) return false; break;
    }
    
    SliceRequest request;
    request.axis = axis;
    request.index = index;
    
    SlicePlan plan = planSlice(header_, request);
    prepareSlicePlan(plan);
    
    if (plan.merged_extents.empty()) return true;
    
    // Calculate total read size and determine batching
    uint64_t totalReadSize = 0;
    for (const auto& ext : plan.merged_extents) {
        totalReadSize += ext.size;
    }
    
    size_t maxBufferBytes = memoryLimitMB * 1024 * 1024;
    if (maxBufferBytes == 0) maxBufferBytes = 1024 * 1024; // at least 1 MB
    
    // If everything fits, read at once
    if (totalReadSize <= maxBufferBytes) {
        std::vector<uint8_t> readBuffer(totalReadSize);
        
        if (numThreads <= 1) {
            if (!readExtents(plan.merged_extents, readBuffer.data())) return false;
        } else {
            if (!readExtentsThreaded(plan.merged_extents, readBuffer.data(), numThreads)) return false;
        }
        
        executeSlice(header_, plan, readBuffer.data(), output);
        return true;
    }
    
    // Batched reading: split merged extents into groups that fit
    uint64_t batchBytes = 0;
    size_t batchStart = 0;
    
    for (size_t i = 0; i <= plan.merged_extents.size(); ++i) {
        bool flushBatch = (i == plan.merged_extents.size());
        if (!flushBatch) {
            uint64_t nextSize = batchBytes + plan.merged_extents[i].size;
            if (nextSize <= maxBufferBytes) {
                batchBytes = nextSize;
                continue;
            }
            if (batchBytes == 0) {
                // Single extent exceeds limit - still read it
                batchBytes = plan.merged_extents[i].size;
                continue;
            }
            flushBatch = true;
        }
        
        if (batchStart < i) {
            // Read batch [batchStart, i)
            size_t batchEnd = flushBatch ? i : i;
            uint64_t bufSize = 0;
            for (size_t j = batchStart; j < batchEnd; ++j) {
                bufSize += plan.merged_extents[j].size;
            }
            std::vector<uint8_t> batchBuffer(bufSize);
            
            // Build sub-extent list
            std::vector<Extent> batchExtents;
            for (size_t j = batchStart; j < batchEnd; ++j) {
                batchExtents.push_back(plan.merged_extents[j]);
            }
            
            if (numThreads <= 1) {
                if (!readExtents(batchExtents, batchBuffer.data())) return false;
            } else {
                if (!readExtentsThreaded(batchExtents, batchBuffer.data(), numThreads)) return false;
            }
            
            // Execute only copies whose merged extent is in this batch
            for (size_t ci = 0; ci < plan.copies.size(); ++ci) {
                uint32_t mi = plan.copy_merged_idx[ci];
                if (mi >= batchStart && mi < batchEnd) {
                    uint64_t batchBufOffset = plan.copy_merged_offset[ci] - plan.merged_buffer_offsets[batchStart];
                    const float* src = reinterpret_cast<const float*>(batchBuffer.data() + batchBufOffset);
                    
                    const auto& copy = plan.copies[ci];
                    for (uint64_t dz = 0; dz < copy.size_z; ++dz) {
                        for (uint64_t dy = 0; dy < copy.size_y; ++dy) {
                            for (uint64_t dx = 0; dx < copy.size_x; ++dx) {
                                uint64_t srcIdx = ((copy.src_off_z + dz) * header_.leaf_y + 
                                                   (copy.src_off_y + dy)) * header_.leaf_x + 
                                                  (copy.src_off_x + dx);
                                uint64_t d = 0;
                                switch (plan.axis) {
                                    case SliceAxis::X:
                                        d = ((copy.base_dst_idx / header_.ny) + dz) * header_.ny + 
                                            (copy.base_dst_idx % header_.ny + dy);
                                        break;
                                    case SliceAxis::Y:
                                        d = ((copy.base_dst_idx / header_.nx) + dz) * header_.nx + 
                                            (copy.base_dst_idx % header_.nx + dx);
                                        break;
                                    case SliceAxis::Z:
                                        d = ((copy.base_dst_idx / header_.nx) + dy) * header_.nx + 
                                            (copy.base_dst_idx % header_.nx + dx);
                                        break;
                                }
                                output[d] = src[srcIdx];
                            }
                        }
                    }
                }
            }
        }
        
        // Reset for next batch
        batchStart = i;
        batchBytes = 0;
        if (!flushBatch) {
            batchBytes = plan.merged_extents[i].size;
        }
    }
    
    return true;
}

// --- readLineX (legacy wrapper) ---
bool ERWT3DReader::readLineX(uint64_t y, uint64_t z, float* output) {
    return readLineX(y, z, output, 1, 2048);
}

// --- readLineX with threading and memory control ---
bool ERWT3DReader::readLineX(uint64_t y, uint64_t z, float* output,
                              int numThreads, size_t memoryLimitMB) {
    if (fd_ < 0) return false;
    
    const uint64_t nx = header_.nx;
    const uint64_t ny = header_.ny;
    const uint64_t nz = header_.nz;
    
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
    
    if (numThreads <= 1) {
        if (!readExtents(mergedExtents, readBuffer.data())) return false;
    } else {
        if (!readExtentsThreaded(mergedExtents, readBuffer.data(), numThreads)) return false;
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

// --- readFull (for tests/small volumes) ---
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
    
    size_t maxBuf = memoryLimitMB * 1024 * 1024;
    if (superBytes + leafBytes > maxBuf) return false;
    
    std::vector<uint8_t> superBuffer(superBytes);
    
    for (uint64_t szi = 0; szi < getSuperGridZ(header_); ++szi) {
        for (uint64_t syi = 0; syi < getSuperGridY(header_); ++syi) {
            for (uint64_t sxi = 0; sxi < getSuperGridX(header_); ++sxi) {
                uint64_t superIdx = (szi * getSuperGridY(header_) + syi) * getSuperGridX(header_) + sxi;
                uint64_t superOffset = header_.data_offset + superIdx * superBytes;
                
                ssize_t bytesRead = pread(fd_, superBuffer.data(), superBytes, superOffset);
                if (bytesRead != static_cast<ssize_t>(superBytes)) return false;
                
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

// --- readFullToFile (streaming) ---
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
    
    size_t maxBuf = memoryLimitMB * 1024 * 1024;
    if (superBytes + leafBytes > maxBuf) return false;
    
    int outFd = open(outputPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (outFd < 0) return false;
    
    uint64_t rawSize = nx * ny * nz * sizeof(float);
    if (ftruncate(outFd, rawSize) != 0) { close(outFd); return false; }
    
    std::vector<uint8_t> superBuffer(superBytes);
    
    for (uint64_t szi = 0; szi < getSuperGridZ(header_); ++szi) {
        for (uint64_t syi = 0; syi < getSuperGridY(header_); ++syi) {
            for (uint64_t sxi = 0; sxi < getSuperGridX(header_); ++sxi) {
                uint64_t superIdx = (szi * getSuperGridY(header_) + syi) * getSuperGridX(header_) + sxi;
                uint64_t superOffset = header_.data_offset + superIdx * superBytes;
                
                ssize_t bytesRead = pread(fd_, superBuffer.data(), superBytes, superOffset);
                if (bytesRead != static_cast<ssize_t>(superBytes)) { close(outFd); return false; }
                
                uint64_t startX = sxi * sx, startY = syi * sy, startZ = szi * sz;
                
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
                                uint64_t globalZ = baseZ + z; if (globalZ >= nz) break;
                                for (uint64_t y = 0; y < ly; ++y) {
                                    uint64_t globalY = baseY + y; if (globalY >= ny) break;
                                    uint64_t globalX = baseX; if (globalX >= nx) break;
                                    uint64_t validLx = std::min(lx, nx - globalX);
                                    uint64_t srcIdx = (z * ly + y) * lx;
                                    uint64_t fileOffset = ((globalZ * ny + globalY) * nx + globalX) * sizeof(float);
                                    
                                    ssize_t written = pwrite(outFd, leafData + srcIdx, validLx * sizeof(float), fileOffset);
                                    if (written != static_cast<ssize_t>(validLx * sizeof(float))) {
                                        close(outFd); return false;
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

// --- Sequential extent read ---
bool ERWT3DReader::readExtents(const std::vector<Extent>& extents, void* buffer) {
    uint8_t* buf = static_cast<uint8_t*>(buffer);
    uint64_t offset = 0;
    
    for (const auto& ext : extents) {
        if (!readOneExtent(ext.offset, ext.size, buf + offset)) return false;
        offset += ext.size;
    }
    
    return true;
}

// --- Threaded extent read via thread pool ---
bool ERWT3DReader::readExtentsThreaded(const std::vector<Extent>& extents, void* buffer, int numThreads) {
    if (numThreads <= 1 || extents.size() <= 1) {
        return readExtents(extents, buffer);
    }
    
    uint8_t* buf = static_cast<uint8_t*>(buffer);
    
    // Compute buffer offsets
    std::vector<uint64_t> offsets(extents.size());
    uint64_t total = 0;
    for (size_t i = 0; i < extents.size(); ++i) {
        offsets[i] = total;
        total += extents[i].size;
    }
    
    ThreadPool pool(std::min(static_cast<size_t>(numThreads), extents.size()));
    std::vector<std::future<bool>> futures;
    
    for (size_t i = 0; i < extents.size(); ++i) {
        futures.push_back(pool.submit([this, &extents, buf, &offsets](size_t idx) -> bool {
            return readOneExtent(extents[idx].offset, extents[idx].size, buf + offsets[idx]);
        }, i));
    }
    
    pool.waitAll();
    
    for (auto& f : futures) {
        if (!f.get()) return false;
    }
    
    return true;
}

} // namespace erwt3d