#include "erwt3d/reader.hpp"
#include "erwt3d/morton.hpp"
#include "erwt3d/thread_pool.hpp"
#include "erwt3d/sb_task.hpp"
#include <algorithm>
#include <fstream>
#include <vector>
#include <cstring>
#include <chrono>
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
    // Read tile directory if present
    if (header_.flags & FLAG_HAS_TILE_DIR) {
        uint64_t totalSB = getTotalSuperblocks(header_);
        uint64_t dirBytes = totalSB * sizeof(uint64_t);
        tileDir_.resize(totalSB);
        pread(fd_, tileDir_.data(), dirBytes, sizeof(header_));
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
    const uint64_t leafBytes = getLeafBytes(header_);
    // Only cache exact leaf-sized blocks (256 bytes) to ensure leaf-level cache semantics
    bool useCache = cache_ && (size == leafBytes);
    if (useCache) {
        if (cache_->get(offset, buffer, size)) return true;
    }
    ssize_t n = pread(fd_, buffer, size, offset);
    if (n != static_cast<ssize_t>(size)) return false;
    if (useCache) {
        cache_->put(offset, buffer, size);
    }
    return true;
}

// --- readSlice (legacy wrapper) ---
bool ERWT3DReader::readSlice(SliceAxis axis, uint64_t index, float* output) {
    return readSlice(axis, index, output, 1, 2048);
}

// --- readSlice with threading and memory control ---
// --- readSlice with backend routing ---
bool ERWT3DReader::readSlice(SliceAxis axis, uint64_t index, float* output,
                              int numThreads, size_t memoryLimitMB) {
    if (fd_ < 0) return false;
    switch (axis) {
        case SliceAxis::X: if (index >= header_.nx) return false; break;
        case SliceAxis::Y: if (index >= header_.ny) return false; break;
        case SliceAxis::Z: if (index >= header_.nz) return false; break;
    }
    if (ioBackend_ == IOBackend::Superblock) {
        return readSliceSB(axis, index, output, numThreads, memoryLimitMB);
    }
    return readSlicePRead(axis, index, output, numThreads, memoryLimitMB);
}

// --- readSlice: extent-based PRead backend ---
bool ERWT3DReader::readSlicePRead(SliceAxis axis, uint64_t index, float* output,
                                   int numThreads, size_t memoryLimitMB) {
    if (fd_ < 0) return false;
    
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
        
        executePreparedSlice(header_, plan, readBuffer.data(),
                            plan.merged_buffer_offsets, plan.merged_extents,
                            0, plan.merged_extents.size(), output);
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
            executePreparedSlice(header_, plan, batchBuffer.data(),
                                plan.merged_buffer_offsets, plan.merged_extents,
                                batchStart, batchEnd, output);
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
    
    size_t maxBuf = memoryLimitMB * 1024 * 1024;
    if (maxBuf == 0) maxBuf = 1024 * 1024;
    
    const uint64_t srcLineOffset = (inLeafZ * ly + inLeafY) * lx;
    
    // If all fits, single batch
    if (totalReadSize <= maxBuf) {
        std::vector<uint8_t> readBuffer(totalReadSize);
        if (numThreads <= 1) {
            if (!readExtents(mergedExtents, readBuffer.data())) return false;
        } else {
            if (!readExtentsThreaded(mergedExtents, readBuffer.data(), numThreads)) return false;
        }
        
        for (size_t i = 0; i < extents.size(); ++i) {
            uint64_t srcOffset = 0, bufferOffset = 0;
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
            for (uint64_t dx = 0; dx < validLx; ++dx)
                output[baseX + dx] = leafData[srcLineOffset + dx];
        }
        return true;
    }
    
    // Batched reading
    uint64_t batchSize = 0;
    size_t batchStart = 0;
    for (size_t i = 0; i <= mergedExtents.size(); ++i) {
        bool flush = (i == mergedExtents.size());
        if (!flush) {
            uint64_t next = batchSize + mergedExtents[i].size;
            if (next <= maxBuf) { batchSize = next; continue; }
            if (batchSize == 0) { batchSize = mergedExtents[i].size; continue; }
            flush = true;
        }
        if (batchStart < i) {
            size_t batchEnd = flush ? i : i;
            uint64_t bufSz = 0;
            for (size_t j = batchStart; j < batchEnd; ++j) bufSz += mergedExtents[j].size;
            std::vector<uint8_t> buf(bufSz);
            std::vector<Extent> be(mergedExtents.begin() + batchStart, mergedExtents.begin() + batchEnd);
            uint64_t bo = 0;
            for (const auto& e : be) { if (!readOneExtent(e.offset, e.size, buf.data() + bo)) return false; bo += e.size; }
            
            bo = 0;
            for (size_t j = batchStart; j < batchEnd; ++j) {
                for (size_t k = 0; k < extents.size(); ++k) {
                    if (extents[k].offset >= mergedExtents[j].offset &&
                        extents[k].offset < mergedExtents[j].end()) {
                        uint64_t leafOff = bo + (extents[k].offset - mergedExtents[j].offset);
                        const float* ld = reinterpret_cast<const float*>(buf.data() + leafOff);
                        uint64_t bx = extentBaseX[k];
                        uint64_t vlx = std::min(lx, nx - bx);
                        for (uint64_t dx = 0; dx < vlx; ++dx)
                            output[bx + dx] = ld[srcLineOffset + dx];
                    }
                }
                bo += mergedExtents[j].size;
            }
        }
        batchStart = i; batchSize = 0;
        if (!flush) batchSize = mergedExtents[i].size;
    }
    
    return true;
}

// --- readLine (axis-generic) ---
bool ERWT3DReader::readLine(SliceAxis axis, uint64_t fixed1, uint64_t fixed2, float* output,
                             int numThreads, size_t memoryLimitMB) {
    switch (axis) {
        case SliceAxis::X: return readLineX(fixed1, fixed2, output, numThreads, memoryLimitMB);
        case SliceAxis::Y: return readLineY(fixed1, fixed2, output, numThreads, memoryLimitMB);
        case SliceAxis::Z: return readLineZ(fixed1, fixed2, output, numThreads, memoryLimitMB);
    }
    return false;
}

// --- readLineY: fixed x,z, varying y ---
bool ERWT3DReader::readLineY(uint64_t x, uint64_t z, float* output,
                              int numThreads, size_t memoryLimitMB) {
    if (fd_ < 0) return false;
    const uint64_t nx = header_.nx, ny = header_.ny, nz = header_.nz;
    if (x >= nx || z >= nz) return false;

    const uint64_t sx = header_.super_x, sy = header_.super_y, sz = header_.super_z;
    const uint64_t lx = header_.leaf_x, ly = header_.leaf_y, lz = header_.leaf_z;
    const uint64_t superBytes = getSuperblockBytes(header_);
    const uint64_t leafBytes = getLeafBytes(header_);
    const uint64_t lpsY = getLeafsPerSuperY(header_);

    uint64_t superX = x / sx, superZ = z / sz;
    uint64_t localX = x % sx, localZ = z % sz;
    uint64_t leafX = localX / lx, leafZ = localZ / lz;
    uint64_t inLeafX = localX % lx, inLeafZ = localZ % lz;

    std::vector<Extent> extents;
    std::vector<uint64_t> extentBaseY;

    for (uint64_t superY = 0; superY < getSuperGridY(header_); ++superY) {
        uint64_t sbIdx = (superZ * getSuperGridY(header_) + superY) * getSuperGridX(header_) + superX;
        uint64_t sbOff = header_.data_offset + sbIdx * superBytes;
        for (uint64_t lyi = 0; lyi < lpsY; ++lyi) {
            uint64_t baseY = superY * sy + lyi * ly;
            if (baseY >= ny) continue;
            uint64_t morton = morton3D(static_cast<uint32_t>(leafX),
                                       static_cast<uint32_t>(lyi),
                                       static_cast<uint32_t>(leafZ));
            extents.emplace_back(sbOff + morton * leafBytes, leafBytes);
            extentBaseY.push_back(baseY);
        }
    }
    if (extents.empty()) return true;

    auto merged = mergeExtents(extents);
    size_t maxBuf = memoryLimitMB * 1024 * 1024;
    if (maxBuf == 0) maxBuf = 1024 * 1024;
    uint64_t srcOff = inLeafZ * ly * lx + inLeafX; // (inLeafZ * ly + 0) * lx + inLeafX
    size_t totalSize = 0; for (auto& e : merged) totalSize += e.size;

    if (totalSize <= maxBuf) {
        std::vector<uint8_t> buf(totalSize);
        if (numThreads <= 1) { if (!readExtents(merged, buf.data())) return false; }
        else { if (!readExtentsThreaded(merged, buf.data(), numThreads)) return false; }
        for (size_t i = 0; i < extents.size(); ++i) {
            uint64_t bo = 0; for (size_t j = 0; j < merged.size(); ++j) {
                if (extents[i].offset >= merged[j].offset && extents[i].offset < merged[j].end()) {
                    const float* ld = reinterpret_cast<const float*>(buf.data() + bo + (extents[i].offset - merged[j].offset));
                    uint64_t by = extentBaseY[i], vly = std::min(ly, ny - by);
                    for (uint64_t dy = 0; dy < vly; ++dy)
                        output[by + dy] = ld[dy * lx + srcOff];
                    break;
                }
                bo += merged[j].size;
            }
        }
        return true;
    }

    // Batched
    uint64_t batchSz = 0; size_t bs = 0;
    for (size_t i = 0; i <= merged.size(); ++i) {
        bool flush = (i == merged.size());
        if (!flush) { uint64_t nxt = batchSz + merged[i].size; if (nxt <= maxBuf) { batchSz = nxt; continue; } if (batchSz == 0) { batchSz = merged[i].size; continue; } flush = true; }
        if (bs < i) {
            size_t be = flush ? i : i; uint64_t bsz = 0;
            for (size_t j = bs; j < be; ++j) bsz += merged[j].size;
            std::vector<uint8_t> buf2(bsz);
            std::vector<Extent> bext(merged.begin() + bs, merged.begin() + be);
            uint64_t bo2 = 0;
            for (const auto& e : bext) { if (!readOneExtent(e.offset, e.size, buf2.data() + bo2)) return false; bo2 += e.size; }
            bo2 = 0;
            for (size_t j = bs; j < be; ++j) {
                for (size_t k = 0; k < extents.size(); ++k) {
                    if (extents[k].offset >= merged[j].offset && extents[k].offset < merged[j].end()) {
                        const float* ld = reinterpret_cast<const float*>(buf2.data() + bo2 + (extents[k].offset - merged[j].offset));
                        uint64_t by = extentBaseY[k], vly = std::min(ly, ny - by);
                        for (uint64_t dy = 0; dy < vly; ++dy)
                            output[by + dy] = ld[dy * lx + srcOff];
                    }
                }
                bo2 += merged[j].size;
            }
        }
        bs = i; batchSz = 0; if (!flush) batchSz = merged[i].size;
    }
    return true;
}

// --- readLineZ: fixed x,y, varying z ---
bool ERWT3DReader::readLineZ(uint64_t x, uint64_t y, float* output,
                              int numThreads, size_t memoryLimitMB) {
    if (fd_ < 0) return false;
    const uint64_t nx = header_.nx, ny = header_.ny, nz = header_.nz;
    if (x >= nx || y >= ny) return false;

    const uint64_t sx = header_.super_x, sy = header_.super_y, sz = header_.super_z;
    const uint64_t lx = header_.leaf_x, ly = header_.leaf_y, lz = header_.leaf_z;
    const uint64_t superBytes = getSuperblockBytes(header_);
    const uint64_t leafBytes = getLeafBytes(header_);
    const uint64_t lpsZ = getLeafsPerSuperZ(header_);

    uint64_t superX = x / sx, superY = y / sy;
    uint64_t localX = x % sx, localY = y % sy;
    uint64_t leafX = localX / lx, leafY = localY / ly;
    uint64_t inLeafX = localX % lx, inLeafY = localY % ly;

    std::vector<Extent> extents;
    std::vector<uint64_t> extentBaseZ;

    for (uint64_t superZ = 0; superZ < getSuperGridZ(header_); ++superZ) {
        uint64_t sbIdx = (superZ * getSuperGridY(header_) + superY) * getSuperGridX(header_) + superX;
        uint64_t sbOff = header_.data_offset + sbIdx * superBytes;
        for (uint64_t lzi = 0; lzi < lpsZ; ++lzi) {
            uint64_t baseZ = superZ * sz + lzi * lz;
            if (baseZ >= nz) continue;
            uint64_t morton = morton3D(static_cast<uint32_t>(leafX),
                                       static_cast<uint32_t>(leafY),
                                       static_cast<uint32_t>(lzi));
            extents.emplace_back(sbOff + morton * leafBytes, leafBytes);
            extentBaseZ.push_back(baseZ);
        }
    }
    if (extents.empty()) return true;

    auto merged = mergeExtents(extents);
    size_t maxBuf = memoryLimitMB * 1024 * 1024;
    if (maxBuf == 0) maxBuf = 1024 * 1024;
    uint64_t srcOff = (inLeafY * lx + inLeafX); // (0 * ly + inLeafY) * lx + inLeafX for z=0
    size_t totalSize = 0; for (auto& e : merged) totalSize += e.size;

    if (totalSize <= maxBuf) {
        std::vector<uint8_t> buf(totalSize);
        if (numThreads <= 1) { if (!readExtents(merged, buf.data())) return false; }
        else { if (!readExtentsThreaded(merged, buf.data(), numThreads)) return false; }
        for (size_t i = 0; i < extents.size(); ++i) {
            uint64_t bo = 0; for (size_t j = 0; j < merged.size(); ++j) {
                if (extents[i].offset >= merged[j].offset && extents[i].offset < merged[j].end()) {
                    const float* ld = reinterpret_cast<const float*>(buf.data() + bo + (extents[i].offset - merged[j].offset));
                    uint64_t bz = extentBaseZ[i], vlz = std::min(lz, nz - bz);
                    for (uint64_t dz = 0; dz < vlz; ++dz)
                        output[bz + dz] = ld[dz * ly * lx + srcOff];
                    break;
                }
                bo += merged[j].size;
            }
        }
        return true;
    }

    // Batched
    uint64_t batchSz = 0; size_t bs = 0;
    for (size_t i = 0; i <= merged.size(); ++i) {
        bool flush = (i == merged.size());
        if (!flush) { uint64_t nxt = batchSz + merged[i].size; if (nxt <= maxBuf) { batchSz = nxt; continue; } if (batchSz == 0) { batchSz = merged[i].size; continue; } flush = true; }
        if (bs < i) {
            size_t be = flush ? i : i; uint64_t bsz = 0;
            for (size_t j = bs; j < be; ++j) bsz += merged[j].size;
            std::vector<uint8_t> buf2(bsz);
            std::vector<Extent> bext(merged.begin() + bs, merged.begin() + be);
            uint64_t bo2 = 0;
            for (const auto& e : bext) { if (!readOneExtent(e.offset, e.size, buf2.data() + bo2)) return false; bo2 += e.size; }
            bo2 = 0;
            for (size_t j = bs; j < be; ++j) {
                for (size_t k = 0; k < extents.size(); ++k) {
                    if (extents[k].offset >= merged[j].offset && extents[k].offset < merged[j].end()) {
                        const float* ld = reinterpret_cast<const float*>(buf2.data() + bo2 + (extents[k].offset - merged[j].offset));
                        uint64_t bz = extentBaseZ[k], vlz = std::min(lz, nz - bz);
                        for (uint64_t dz = 0; dz < vlz; ++dz)
                            output[bz + dz] = ld[dz * ly * lx + srcOff];
                    }
                }
                bo2 += merged[j].size;
            }
        }
        bs = i; batchSz = 0; if (!flush) batchSz = merged[i].size;
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

// --- Superblock-level I/O backend ---

bool ERWT3DReader::readSliceSB(SliceAxis axis, uint64_t index, float* output,
                                int numThreads, size_t memoryLimitMB) {
    const uint64_t sbBytesVal = getSuperblockBytes(header_);
    size_t maxBytes = memoryLimitMB * 1024ULL * 1024ULL;
    if (sbBytesVal > maxBytes) return false;
    size_t required = sbBytesVal;
    if (sbParallelMode_ == SBParallelMode::ParallelRead && numThreads > 1) {
        required *= static_cast<size_t>(numThreads);
    }
    if (required > maxBytes) return false;

    auto planStart = std::chrono::high_resolution_clock::now();

    // Try X-panel fast path
    if (axis == SliceAxis::X && hasXPanels(header_)) {
        bool panelOk;
        if (sbParallelMode_ == SBParallelMode::ParallelRead && numThreads > 1) {
            panelOk = tryReadSliceXPanelsParallel(fd_, header_, index, output, numThreads,
                                                   profileIO_ ? &lastProfile_ : nullptr);
        } else {
            panelOk = tryReadSliceXPanels(fd_, header_, index, output,
                                           profileIO_ ? &lastProfile_ : nullptr);
        }
        if (panelOk) {
            auto planEnd = std::chrono::high_resolution_clock::now();
            double planMs = std::chrono::duration<double, std::milli>(planEnd - planStart).count();
            lastProfile_.plan_time_ms = planMs;
            return true;
        }
        // Fall through to SB if panel miss
    }

    SBTaskPlan plan;
    switch (axis) {
        case SliceAxis::Z: plan = buildSBPlanZ(header_, index); break;
        case SliceAxis::Y: plan = buildSBPlanY(header_, index); break;
        case SliceAxis::X: plan = buildSBPlanX(header_, index); break;
    }
    if (plan.tasks.empty()) return true;

    // Apply tile directory offset translation
    if (!tileDir_.empty()) {
        uint64_t sbBV = getSuperblockBytes(header_);
        for (auto& task : plan.tasks) {
            uint64_t sbIdx = (task.file_offset - header_.data_offset) / sbBV;
            if (sbIdx < tileDir_.size()) task.file_offset = tileDir_[sbIdx];
        }
    }

    // Apply task ordering if requested
    if (sbTaskOrder_ == SBTaskOrder::FileOffset) {
        sortTasksByFileOffset(plan);
    }

    auto planEnd = std::chrono::high_resolution_clock::now();
    double planMs = std::chrono::duration<double, std::milli>(planEnd - planStart).count();

    lastProfile_ = IOProfile{};
    lastProfile_.plan_time_ms = planMs;
    lastProfile_.superblocks_touched = plan.superblocks_touched;
    lastProfile_.pread_calls = plan.pread_calls;
    lastProfile_.bytes_read = plan.bytes_read;
    lastProfile_.output_bytes = plan.output_bytes;

    bool ok;
    if (sbReadMode_ == SBReadMode::HDDReadWindow) {
        ok = executeSBPlanHDDReadWindow(fd_, plan, header_, output, numThreads,
                                        memoryLimitMB, hddReadWindowCfg_,
                                        profileIO_ ? &lastProfile_ : nullptr,
                                        pinThreads_);
    } else if (sbReadMode_ == SBReadMode::RunBatch) {
        ok = executeSBPlanRunBatch(fd_, plan, header_, output, numThreads,
                                    memoryLimitMB, profileIO_ ? &lastProfile_ : nullptr,
                                    pinThreads_);
    } else if (sbReadMode_ == SBReadMode::LeafIndex) {
        ok = executeSBPlanLeafIndex(fd_, plan, header_, output, numThreads,
                                     memoryLimitMB, leafMergeBytes_,
                                     profileIO_ ? &lastProfile_ : nullptr,
                                     pinThreads_);
    } else if (sbParallelMode_ == SBParallelMode::ParallelRead && numThreads > 1) {
        ok = executeSBPlanParallelRead(fd_, plan, header_, output, numThreads,
                                        profileIO_ ? &lastProfile_ : nullptr,
                                        sbSchedule_, pinThreads_);
    } else {
        ok = executeSBPlanSerial(fd_, plan, header_, output,
                                 profileIO_ ? &lastProfile_ : nullptr);
    }

    return ok;
}

bool ERWT3DReader::readSlicesBatch(const std::vector<SliceBatchRequest>& requests,
                                    int numThreads, size_t memoryLimitMB,
                                    const HDDReadWindowConfig& wcfg,
                                    SBBatchProfile* profile) {
    if (fd_ < 0 || requests.empty()) return false;
    std::vector<SBTaskPlan> plans; plans.reserve(requests.size());
    std::vector<float*> outputs; outputs.reserve(requests.size());
    std::vector<const SBTaskPlan*> pp; pp.reserve(requests.size());
    for (const auto& r : requests) {
        SBTaskPlan p;
        switch (r.axis) {
            case SliceAxis::Z: p = buildSBPlanZ(header_, r.index); break;
            case SliceAxis::Y: p = buildSBPlanY(header_, r.index); break;
            case SliceAxis::X: p = buildSBPlanX(header_, r.index); break;
        }
        if (sbTaskOrder_ == SBTaskOrder::FileOffset) sortTasksByFileOffset(p);
        plans.push_back(std::move(p)); outputs.push_back(r.output);
    }
    // Apply tile directory offset translation
    if (!tileDir_.empty()) {
        uint64_t sbBV = getSuperblockBytes(header_);
        for (auto& p : plans)
            for (auto& t : p.tasks) {
                uint64_t si = (t.file_offset - header_.data_offset) / sbBV;
                if (si < tileDir_.size()) t.file_offset = tileDir_[si];
            }
    }
    for (auto& p : plans) pp.push_back(&p);
    return executeSBBatchHDD(fd_, buildSBBatchPlan(pp), header_, outputs.data(),
                              numThreads, memoryLimitMB, wcfg, pinThreads_, profile);
}

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