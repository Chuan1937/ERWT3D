#include "erwt3d/reader.hpp"
#include "erwt3d/morton.hpp"
#include "erwt3d/raw_layout.hpp"
#include "erwt3d/raw_x_aux.hpp"
#include "erwt3d/thread_pool.hpp"
#include "erwt3d/sb_task.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <vector>
#include <cstring>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unordered_map>

#ifdef ERWT3D_HAVE_LZ4
#include <lz4.h>
#endif

namespace erwt3d {

namespace {

inline void adviseSequential(int fd, uint64_t offset = 0, uint64_t bytes = 0) {
#if defined(POSIX_FADV_SEQUENTIAL)
    posix_fadvise(fd, static_cast<off_t>(offset), static_cast<off_t>(bytes), POSIX_FADV_SEQUENTIAL);
#else
    (void)fd;
    (void)offset;
    (void)bytes;
#endif
}

inline void adviseWillNeed(int fd, uint64_t offset, uint64_t bytes) {
#if defined(POSIX_FADV_WILLNEED)
    posix_fadvise(fd, static_cast<off_t>(offset), static_cast<off_t>(bytes), POSIX_FADV_WILLNEED);
#else
    (void)fd;
    (void)offset;
    (void)bytes;
#endif
}

} // namespace

ERWT3DReader::ERWT3DReader(const std::string& path, size_t cacheMB, bool useMmap)
    : path_(path), fd_(-1), cacheMB_(cacheMB), useMmap_(useMmap) {
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

    validateLeafOpRanges(header_);

    if (cacheMB > 0) {
        cache_ = std::make_unique<LeafCache>(cacheMB * 1024 * 1024);
    }

    // Load compression index if file is compressed
    compressed_ = isCompressed(header_);
    if (compressed_) {
        uint64_t idxOffset = getCompressionIndexOffset(header_);
        uint64_t idxCount = getCompressedBlockCount(header_);
        if (idxOffset > 0 && idxCount > 0) {
            compIndex_.resize(idxCount);
            ssize_t idxBytes = idxCount * sizeof(CompressedBlockIndex);
            if (pread(fd_, compIndex_.data(), idxBytes, idxOffset) != idxBytes) {
                compIndex_.clear();
                compressed_ = false;
            }
        } else {
            compressed_ = false;
        }
    }

    if (useMmap_) {
        struct stat st;
        if (fstat(fd_, &st) == 0) {
            mmapSize_ = st.st_size;
            mmapData_ = mmap(nullptr, mmapSize_, PROT_READ, MAP_PRIVATE, fd_, 0);
            if (mmapData_ == MAP_FAILED) {
                mmapData_ = nullptr;
                mmapSize_ = 0;
            } else {
                madvise(mmapData_, mmapSize_, MADV_SEQUENTIAL);
            }
        }
    }

    if (hasXPSidecar(header_)) {
        loadSidecar_();
    }

    initRawXAux_();
}

ERWT3DReader::~ERWT3DReader() {
    if (mmapData_ && mmapSize_ > 0) {
        munmap(mmapData_, mmapSize_);
    }
    if (rawXAuxFd_ >= 0) {
        close(rawXAuxFd_);
    }
    if (fd_ >= 0) {
        close(fd_);
    }
    if (xpFd_ >= 0) {
        close(xpFd_);
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

    if (mmapData_ && offset + size <= mmapSize_) {
        memcpy(buffer, static_cast<const uint8_t*>(mmapData_) + offset, size);
    } else {
        ssize_t n = pread(fd_, buffer, size, offset);
        if (n != static_cast<ssize_t>(size)) return false;
    }

    if (useCache) {
        cache_->put(offset, buffer, size);
    }
    return true;
}

bool ERWT3DReader::readSuperblock(uint64_t sbIdx, void* buffer) {
    if (fd_ < 0) return false;
    uint64_t sbBytes = getSuperblockBytes(header_);

    if (compressed_ && sbIdx < compIndex_.size()) {
        const auto& entry = compIndex_[sbIdx];
        if (entry.is_compressed) {
#ifdef ERWT3D_HAVE_LZ4
            std::vector<uint8_t> compBuf(entry.compressed_size);
            ssize_t n = pread(fd_, compBuf.data(), entry.compressed_size, entry.file_offset);
            if (n != static_cast<ssize_t>(entry.compressed_size)) return false;
            int decSize = LZ4_decompress_safe(
                reinterpret_cast<const char*>(compBuf.data()),
                reinterpret_cast<char*>(buffer),
                static_cast<int>(entry.compressed_size),
                static_cast<int>(sbBytes));
            if (decSize != static_cast<int>(sbBytes)) return false;
#else
            return false;
#endif
        } else {
            ssize_t n = pread(fd_, buffer, sbBytes, entry.file_offset);
            if (n != static_cast<ssize_t>(sbBytes)) return false;
        }
        return true;
    }

    // Uncompressed: use direct pread
    uint64_t offset = header_.data_offset + sbIdx * sbBytes;
    ssize_t n = pread(fd_, buffer, sbBytes, offset);
    return n == static_cast<ssize_t>(sbBytes);
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
    return readSliceSB(axis, index, output, numThreads, memoryLimitMB);
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
            size_t batchEnd = i;
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
    return readLineBatched(extents, extentBaseY, inLeafZ * ly * lx + inLeafX, lx, ny, ly, output, numThreads, memoryLimitMB);
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
    return readLineBatched(extents, extentBaseZ, inLeafY * lx + inLeafX, ly * lx, nz, lz, output, numThreads, memoryLimitMB);
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
                
                if (!readSuperblock(superIdx, superBuffer.data())) return false;
                
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
                                        uint64_t dstIdx = (globalX * ny + globalY) * nz + globalZ;
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

// --- readFullToFile (streaming, mmap output) ---
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
    
    // mmap output file for scatter writes without pwrite overhead
    float* outMap = static_cast<float*>(mmap(nullptr, rawSize, PROT_WRITE, MAP_SHARED, outFd, 0));
    if (outMap == MAP_FAILED) {
        // Fallback to pwrite if mmap fails
        std::vector<uint8_t> superBuffer(superBytes);
        for (uint64_t szi = 0; szi < getSuperGridZ(header_); ++szi) {
            for (uint64_t syi = 0; syi < getSuperGridY(header_); ++syi) {
                for (uint64_t sxi = 0; sxi < getSuperGridX(header_); ++sxi) {
                    uint64_t superIdx = (szi * getSuperGridY(header_) + syi) * getSuperGridX(header_) + sxi;
                    ssize_t rd = pread(fd_, superBuffer.data(), superBytes, header_.data_offset + superIdx * superBytes);
                    if (rd != static_cast<ssize_t>(superBytes)) { close(outFd); return false; }
                    uint64_t startX = sxi * sx, startY = syi * sy, startZ = szi * sz;
                    for (uint64_t lzi = 0; lzi < leafsPerSuperZ; ++lzi) {
                        for (uint64_t lyi = 0; lyi < leafsPerSuperY; ++lyi) {
                            for (uint64_t lxi = 0; lxi < leafsPerSuperX; ++lxi) {
                                uint64_t leafMorton = morton3D(lxi, lyi, lzi);
                                const float* ld = reinterpret_cast<const float*>(superBuffer.data() + leafMorton * leafBytes);
                                uint64_t bx = startX + lxi * lx, by = startY + lyi * ly, bz = startZ + lzi * lz;
                                uint64_t vx = std::min(lx, nx - bx);
                                for (uint64_t z = 0; z < lz; ++z) {
                                    uint64_t gz = bz + z; if (gz >= nz) break;
                                    for (uint64_t y = 0; y < ly; ++y) {
                                        uint64_t gy = by + y; if (gy >= ny) break;
                                for (uint64_t x = 0; x < vx; ++x) {
                                    uint64_t gx = bx + x;
                                    uint64_t off = ((gx * ny + gy) * nz + gz) * sizeof(float);
                                    float v = ld[(z * ly + y) * lx + x];
                                    pwrite(outFd, &v, sizeof(float), off);
                                }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        close(outFd);
        return true;
    }
    
    madvise(outMap, rawSize, MADV_SEQUENTIAL);
    
    std::vector<uint8_t> superBuffer(superBytes);
    
    for (uint64_t szi = 0; szi < getSuperGridZ(header_); ++szi) {
        for (uint64_t syi = 0; syi < getSuperGridY(header_); ++syi) {
            for (uint64_t sxi = 0; sxi < getSuperGridX(header_); ++sxi) {
                uint64_t superIdx = (szi * getSuperGridY(header_) + syi) * getSuperGridX(header_) + sxi;
                
                if (!readSuperblock(superIdx, superBuffer.data())) {
                    munmap(outMap, rawSize); close(outFd); return false;
                }
                
                uint64_t startX = sxi * sx, startY = syi * sy, startZ = szi * sz;
                
                for (uint64_t lzi = 0; lzi < leafsPerSuperZ; ++lzi) {
                    for (uint64_t lyi = 0; lyi < leafsPerSuperY; ++lyi) {
                        for (uint64_t lxi = 0; lxi < leafsPerSuperX; ++lxi) {
                            uint64_t leafMorton = morton3D(lxi, lyi, lzi);
                            const float* leafData = reinterpret_cast<const float*>(
                                superBuffer.data() + leafMorton * leafBytes);
                            
                            uint64_t baseX = startX + lxi * lx;
                            uint64_t baseY = startY + lyi * ly;
                            uint64_t baseZ = startZ + lzi * lz;
                            uint64_t validLx = std::min(lx, nx - baseX);
                            
                            for (uint64_t z = 0; z < lz; ++z) {
                                uint64_t globalZ = baseZ + z; if (globalZ >= nz) break;
                                for (uint64_t y = 0; y < ly; ++y) {
                                    uint64_t globalY = baseY + y; if (globalY >= ny) break;
                                    for (uint64_t x = 0; x < validLx; ++x) {
                                        uint64_t globalX = baseX + x;
                                        uint64_t srcIdx = (z * ly + y) * lx + x;
                                        uint64_t dstIdx = (globalX * ny + globalY) * nz + globalZ;
                                        outMap[dstIdx] = leafData[srcIdx];
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    
    msync(outMap, rawSize, MS_ASYNC);
    munmap(outMap, rawSize);
    if (close(outFd) != 0) return false;
    return true;
}

// --- Raw X auxiliary (full-coverage uncompressed X-plane region) ---

void ERWT3DReader::initRawXAux_() {
    if (!hasRawXAux(header_)) return;

    RawXAuxRegion region;
    region.offset = getRawXAuxOffset(header_);
    region.bytes = getRawXAuxBytes(header_);
    region.plane_bytes = getRawXAuxPlaneBytes(header_);
    region.version = getRawXAuxVersion(header_);

    struct stat st;
    if (fstat(fd_, &st) != 0) return;
    uint64_t fileSize = static_cast<uint64_t>(st.st_size);

    // Compute minimum offset: must be after all file content
    uint64_t mainEnd = sizeof(header_);
    if (compressed_ && !compIndex_.empty()) {
        for (const auto& entry : compIndex_) {
            uint64_t end = entry.file_offset + entry.compressed_size;
            if (end > mainEnd) mainEnd = end;
        }
        // Also account for the compression index itself
        uint64_t idxEnd = getCompressionIndexOffset(header_) +
                          getCompressedBlockCount(header_) * sizeof(CompressedBlockIndex);
        if (idxEnd > mainEnd) mainEnd = idxEnd;
    } else {
        uint64_t sbTotal = 0;
        if (checkedMulU64(getTotalSuperblocks(header_), getSuperblockBytes(header_), sbTotal))
            mainEnd = header_.data_offset + sbTotal;
    }
    if (hasXPanels(header_)) {
        uint64_t panelEnd = getPanelDataOffset(header_) + getPanelStorageBytes(header_);
        if (panelEnd > mainEnd) mainEnd = panelEnd;
        uint64_t panelIdxEnd = getPanelIndexOffset(header_) + getTotalSuperblocks(header_) * sizeof(uint64_t);
        if (panelIdxEnd > mainEnd) mainEnd = panelIdxEnd;
    }
    if (hasXPlanes(header_)) {
        uint64_t xPlaneTotal = 0;
        if (checkedMulU64(getXPlaneCount(header_), header_.ny * header_.nz * sizeof(float), xPlaneTotal))
            { uint64_t xpEnd = getXPlaneOffset(header_) + xPlaneTotal; if (xpEnd > mainEnd) mainEnd = xpEnd; }
    }

    uint64_t minimumOffset = (mainEnd + RAW_X_AUX_ALIGN - 1) & ~(RAW_X_AUX_ALIGN - 1);

    auto err = validateRawXAuxRegion(fileSize, minimumOffset,
                                      header_.nx, header_.ny, header_.nz, region);
    if (err != RawXAuxValidationError::None) {
        std::cerr << "Warning: Raw X auxiliary validation failed: "
                  << rawXAuxValidationErrorStr(err)
                  << " — falling back to main file reader" << std::endl;
        return;
    }

    rawXAuxOffset_ = region.offset;
    rawXAuxBytes_ = region.bytes;
    rawXAuxPlaneBytes_ = region.plane_bytes;

    int flags = O_RDONLY;
    if (rawXAuxUseDirect_) {
        flags |= O_DIRECT;
    }
    rawXAuxFd_ = open(path_.c_str(), flags);
    if (rawXAuxFd_ < 0) return;

    rawXAuxAvailable_ = true;
}

bool ERWT3DReader::tryReadSliceRawXAux_(uint64_t x, float* output) {
    if (!rawXAuxAvailable_ || x >= header_.nx) return false;

    uint64_t planeBytes = header_.ny * header_.nz * sizeof(float);
    uint64_t offset = rawXAuxOffset_ + x * rawXAuxPlaneBytes_;

    if (rawXAuxUseDirect_) {
        const uint64_t alignedSize = (planeBytes + 511) & ~511ULL;
        if (rawXAuxDirectBuf_.size() < alignedSize)
            rawXAuxDirectBuf_.resize(alignedSize);
        if (!readFullyAt(rawXAuxFd_, rawXAuxDirectBuf_.data(), alignedSize, offset))
            return false;
        std::memcpy(output, rawXAuxDirectBuf_.data(), planeBytes);
    } else {
        if (!readFullyAt(rawXAuxFd_, output, planeBytes, offset))
            return false;
    }

    posix_fadvise(rawXAuxFd_, static_cast<off_t>(offset),
                  static_cast<off_t>(planeBytes), POSIX_FADV_DONTNEED);

    return true;
}

bool ERWT3DReader::tryReadBatchRawXAux_(
    const std::vector<SliceBatchRequest>& requests,
    std::vector<bool>& handled) {

    if (!rawXAuxAvailable_ || requests.empty()) return false;

    struct RawXAuxTask {
        uint64_t x;
        uint64_t file_offset;
        float* output;
        size_t req_idx;
    };

    std::vector<RawXAuxTask> tasks;
    tasks.reserve(requests.size());
    bool anyHit = false;

    for (size_t i = 0; i < requests.size(); ++i) {
        handled[i] = false;
        if (requests[i].axis != SliceAxis::X) continue;
        uint64_t x = requests[i].index;
        if (x >= header_.nx) continue;
        anyHit = true;
        tasks.push_back({x, rawXAuxOffset_ + x * rawXAuxPlaneBytes_,
                         requests[i].output, i});
    }

    if (!anyHit) return false;
    if (tasks.empty()) return true;

    uint64_t planeBytes = header_.ny * header_.nz * sizeof(float);

    // Sort by file offset for sequential read
    std::sort(tasks.begin(), tasks.end(),
              [](const RawXAuxTask& a, const RawXAuxTask& b) {
                  return a.file_offset < b.file_offset;
              });

    const uint64_t MAX_WINDOW = 256ULL * 1024 * 1024; // 256 MB max window

    size_t i = 0;
    while (i < tasks.size()) {
        uint64_t wstart = tasks[i].file_offset;
        uint64_t wend = wstart + planeBytes;
        size_t j = i + 1;

        // Merge adjacent or gapped planes into one window
        while (j < tasks.size()) {
            uint64_t next_offset = tasks[j].file_offset;
            if (next_offset > wend + planeBytes) break; // gap > 1 plane
            uint64_t proposed_end = next_offset + planeBytes;
            if (proposed_end - wstart > MAX_WINDOW) break;
            wend = proposed_end;
            ++j;
        }

        uint64_t wsize = wend - wstart;

        if (rawXAuxUseDirect_) {
            const uint64_t alignedSz = (wsize + 511) & ~511ULL;
            if (rawXAuxDirectBuf_.size() < alignedSz)
                rawXAuxDirectBuf_.resize(alignedSz);
            if (!readFullyAt(rawXAuxFd_, rawXAuxDirectBuf_.data(), alignedSz, wstart))
                return false;
            for (size_t k = i; k < j; ++k) {
                uint64_t off_in_window = tasks[k].file_offset - wstart;
                std::memcpy(tasks[k].output, rawXAuxDirectBuf_.data() + off_in_window, planeBytes);
                handled[tasks[k].req_idx] = true;
            }
        } else {
            if (rawXAuxWindowBuf_.size() < wsize)
                rawXAuxWindowBuf_.resize(wsize);
            if (!readFullyAt(rawXAuxFd_, rawXAuxWindowBuf_.data(), wsize, wstart))
                return false;
            for (size_t k = i; k < j; ++k) {
                uint64_t off_in_window = tasks[k].file_offset - wstart;
                std::memcpy(tasks[k].output, rawXAuxWindowBuf_.data() + off_in_window, planeBytes);
                handled[tasks[k].req_idx] = true;
            }
        }

        posix_fadvise(rawXAuxFd_, static_cast<off_t>(wstart),
                      static_cast<off_t>(wsize), POSIX_FADV_DONTNEED);

        i = j;
    }

    return true;
}

// --- X-plane sidecar ---

void ERWT3DReader::loadSidecar_() {
    std::string xpPath = path_ + ".xp";
    xpFd_ = open(xpPath.c_str(), O_RDONLY);
    if (xpFd_ < 0) return;

    if (pread(xpFd_, &xpHeader_, sizeof(xpHeader_), 0) != sizeof(xpHeader_)) {
        close(xpFd_); xpFd_ = -1; return;
    }
    if (std::memcmp(xpHeader_.magic, XPSIDECAR_MAGIC, 8) != 0 ||
        xpHeader_.version != XPSIDECAR_VERSION ||
        xpHeader_.nx != header_.nx || xpHeader_.ny != header_.ny ||
        xpHeader_.nz != header_.nz) {
        close(xpFd_); xpFd_ = -1; return;
    }

    uint64_t idxBytes = xpHeader_.total_chunks * sizeof(XPChunkIndex);
    if (idxBytes == 0) {
        close(xpFd_); xpFd_ = -1; return;
    }
    xpIndex_.resize(xpHeader_.total_chunks);
    if (pread(xpFd_, xpIndex_.data(), idxBytes, xpHeader_.index_offset) != static_cast<ssize_t>(idxBytes)) {
        close(xpFd_); xpFd_ = -1; return;
    }
    xpAvailable_ = true;
}

bool ERWT3DReader::tryReadSliceXPSidecar_(uint64_t x, float* output, IOProfile* profile) {
    uint32_t stride = xpHeader_.stride;
    uint64_t planeIdx = x / stride;
    uint32_t cpp = xpHeader_.chunks_per_plane;
    uint64_t ny = xpHeader_.ny;

    adviseSequential(xpFd_);

    uint64_t totalRead = 0;

    for (uint32_t c = 0; c < cpp; ++c) {
        const XPChunkIndex& ci = xpIndex_[planeIdx * cpp + c];
        if (ci.compressed_size == 0) continue;

        if (xpCompBuf_.size() < ci.compressed_size)
            xpCompBuf_.resize(ci.compressed_size);
        ssize_t n = pread(xpFd_, xpCompBuf_.data(), ci.compressed_size, ci.chunk_offset);
        if (n != static_cast<ssize_t>(ci.compressed_size)) return false;
        totalRead += ci.compressed_size;

        if (xpRawBuf_.size() < ci.raw_size)
            xpRawBuf_.resize(ci.raw_size);
        if (xpHeader_.compression == 1) {
#ifdef ERWT3D_HAVE_LZ4
            int dec = LZ4_decompress_safe(
                reinterpret_cast<const char*>(xpCompBuf_.data()),
                reinterpret_cast<char*>(xpRawBuf_.data()),
                static_cast<int>(ci.compressed_size),
                static_cast<int>(ci.raw_size));
            if (dec != static_cast<int>(ci.raw_size)) return false;
#else
            return false;
#endif
        } else {
            std::memcpy(xpRawBuf_.data(), xpCompBuf_.data(), ci.raw_size);
        }

        uint64_t zStart = static_cast<uint64_t>(c) * xpHeader_.chunk_z_rows;
        uint64_t rowsInChunk = std::min(ci.raw_size / (ny * sizeof(float)),
                                         xpHeader_.nz - zStart);
        const float* chunk = reinterpret_cast<const float*>(xpRawBuf_.data());
        transposeZYToYZ(chunk, output + zStart, rowsInChunk, ny, xpHeader_.nz);
    }

    if (profile) {
        profile->panel_hit = true;
        profile->pread_calls = cpp;
        profile->bytes_read = totalRead;
        profile->output_bytes = ny * xpHeader_.nz * sizeof(float);
        profile->superblocks_touched = 1;
    }
    return true;
}

bool ERWT3DReader::tryReadBatchXPSidecar_(const std::vector<SliceBatchRequest>& requests,
                                            std::vector<bool>& handled) {
    if (!xpAvailable_ || requests.empty()) return false;

    uint32_t stride = xpHeader_.stride;
    uint32_t cpp = xpHeader_.chunks_per_plane;
    uint64_t ny = xpHeader_.ny;

    // Collect all sidecar-hit chunk tasks
    struct ChunkTask {
        uint64_t chunk_offset;
        uint32_t compressed_size;
        uint32_t raw_size;
        uint32_t chunk_idx_in_plane;  // c
        size_t request_idx;           // which request this belongs to
        uint64_t plane_idx;
    };
    std::vector<ChunkTask> tasks;
    bool anyHit = false;

    for (size_t i = 0; i < requests.size(); ++i) {
        handled[i] = false;
        if (requests[i].axis != SliceAxis::X) continue;
        uint64_t x = requests[i].index;
        if (x % stride != 0) continue;
        uint64_t planeIdx = x / stride;
        if (planeIdx >= xpHeader_.plane_count) continue;

        anyHit = true;
        for (uint32_t c = 0; c < cpp; ++c) {
            const XPChunkIndex& ci = xpIndex_[planeIdx * cpp + c];
            if (ci.compressed_size == 0) continue;
            tasks.push_back({ci.chunk_offset, ci.compressed_size, ci.raw_size,
                             c, i, planeIdx});
        }
    }

    if (!anyHit) return false;
    if (tasks.empty()) {
        // All hits but all chunks empty — mark as handled
        for (size_t i = 0; i < requests.size(); ++i) {
            if (requests[i].axis == SliceAxis::X &&
                requests[i].index % stride == 0 &&
                requests[i].index / stride < xpHeader_.plane_count) {
                handled[i] = true;
            }
        }
        return true;
    }

    // Sort by chunk_offset for sequential disk access
    std::sort(tasks.begin(), tasks.end(), [](const ChunkTask& a, const ChunkTask& b) {
        return a.chunk_offset < b.chunk_offset;
    });

    adviseSequential(xpFd_);

    // Process with window merging: read contiguous chunks in one pread
    uint64_t totalRead = 0;
    size_t i = 0;
    while (i < tasks.size()) {
        // Find contiguous run
        size_t j = i;
        uint64_t runOff = tasks[i].chunk_offset;
        uint64_t runEnd = runOff + tasks[i].compressed_size;
        while (j + 1 < tasks.size() &&
               tasks[j + 1].chunk_offset <= runEnd + 4096) {  // 4KB gap tolerance
            runEnd = tasks[j + 1].chunk_offset + tasks[j + 1].compressed_size;
            j++;
        }

        uint64_t runSize = runEnd - runOff;
        if (xpCompBuf_.size() < runSize) xpCompBuf_.resize(runSize);
        ssize_t n = pread(xpFd_, xpCompBuf_.data(), runSize, runOff);
        if (n != static_cast<ssize_t>(runSize)) return false;
        totalRead += runSize;

        // Decompress each chunk in the run and scatter to output
        for (size_t k = i; k <= j; ++k) {
            const auto& t = tasks[k];
            uint64_t bufOff = t.chunk_offset - runOff;
            const uint8_t* compData = xpCompBuf_.data() + bufOff;

            if (xpRawBuf_.size() < t.raw_size) xpRawBuf_.resize(t.raw_size);
            if (xpHeader_.compression == 1) {
#ifdef ERWT3D_HAVE_LZ4
                int dec = LZ4_decompress_safe(
                    reinterpret_cast<const char*>(compData),
                    reinterpret_cast<char*>(xpRawBuf_.data()),
                    static_cast<int>(t.compressed_size),
                    static_cast<int>(t.raw_size));
                if (dec != static_cast<int>(t.raw_size)) return false;
#else
                return false;
#endif
            } else {
                std::memcpy(xpRawBuf_.data(), compData, t.raw_size);
            }

            float* output = requests[t.request_idx].output;
            uint64_t zStart = static_cast<uint64_t>(t.chunk_idx_in_plane) * xpHeader_.chunk_z_rows;
            uint64_t rowsInChunk = std::min(t.raw_size / (ny * sizeof(float)),
                                             xpHeader_.nz - zStart);
            const float* chunk = reinterpret_cast<const float*>(xpRawBuf_.data());
            transposeZYToYZ(chunk, output + zStart, rowsInChunk, ny, xpHeader_.nz);
            handled[t.request_idx] = true;
        }

        i = j + 1;
    }

    return true;
}

// --- Superblock-level I/O backend ---

bool ERWT3DReader::readSliceSB(SliceAxis axis, uint64_t index, float* output,
                                int numThreads, size_t memoryLimitMB) {
    const uint64_t sbBytesVal = getSuperblockBytes(header_);
    size_t maxBytes = memoryLimitMB * 1024ULL * 1024ULL;
    if (sbBytesVal > maxBytes) return false;
    size_t required = sbBytesVal;
    if (required > maxBytes) return false;

    auto planStart = std::chrono::high_resolution_clock::now();

    // Try raw X auxiliary fast path (full-coverage uncompressed X-plane region)
    if (axis == SliceAxis::X && rawXAuxAvailable_) {
        auto readStart = std::chrono::high_resolution_clock::now();
        bool ok = tryReadSliceRawXAux_(index, output);
        auto readEnd = std::chrono::high_resolution_clock::now();
        if (ok) {
            auto planEnd = std::chrono::high_resolution_clock::now();
            if (profileIO_) {
                lastProfile_ = IOProfile{};
                lastProfile_.panel_hit = true;
                lastProfile_.pread_calls = 1;
                lastProfile_.bytes_read = header_.ny * header_.nz * sizeof(float);
                lastProfile_.output_bytes = lastProfile_.bytes_read;
                lastProfile_.superblocks_touched = 1;
                lastProfile_.plan_time_ms = std::chrono::duration<double, std::milli>(planEnd - readEnd).count();
                lastProfile_.read_time_ms = std::chrono::duration<double, std::milli>(readEnd - readStart).count();
            }
            return true;
        }
    }

    // Try X-plane sidecar fast path (compressed sidecar file)
    if (axis == SliceAxis::X && xpAvailable_) {
        uint32_t stride = xpHeader_.stride;
        if (index % stride == 0) {
            uint64_t planeIdx = index / stride;
            if (planeIdx < xpHeader_.plane_count) {
                auto readStart = std::chrono::high_resolution_clock::now();
                bool ok = tryReadSliceXPSidecar_(index, output,
                                                  profileIO_ ? &lastProfile_ : nullptr);
                auto readEnd = std::chrono::high_resolution_clock::now();
                if (ok) {
                    auto planEnd = std::chrono::high_resolution_clock::now();
                    double planMs = std::chrono::duration<double, std::milli>(planEnd - planStart).count();
                    double readMs = std::chrono::duration<double, std::milli>(readEnd - readStart).count();
                    if (profileIO_) {
                        lastProfile_.plan_time_ms = planMs - readMs;
                        lastProfile_.read_time_ms = readMs;
                    }
                    return true;
                }
            }
        }
    }

    // Try X-plane fast path (single pread for entire X slice)
    if (axis == SliceAxis::X && hasXPlanes(header_)) {
        uint32_t stride = getXPlaneStride(header_);
        if (index % stride == 0) {
                uint64_t planeIdx = index / stride;
                uint64_t planeCount = getXPlaneCount(header_);
                if (planeIdx < planeCount) {
                    uint64_t planeBytes = header_.ny * header_.nz * sizeof(float);
                    uint64_t off = getXPlaneOffset(header_) + planeIdx * planeBytes;
                    adviseWillNeed(fd_, off, planeBytes);
                    auto readStart = std::chrono::high_resolution_clock::now();
                    xPlaneRawBuf_.resize(header_.ny * header_.nz);
                    ssize_t n = pread(fd_, xPlaneRawBuf_.data(), planeBytes, off);
                auto readEnd = std::chrono::high_resolution_clock::now();
                if (n == static_cast<ssize_t>(planeBytes)) {
                    transposeZYToYZ(xPlaneRawBuf_.data(), output, header_.nz, header_.ny, header_.nz);
                    auto planEnd = std::chrono::high_resolution_clock::now();
                    double planMs = std::chrono::duration<double, std::milli>(planEnd - planStart).count();
                    double readMs = std::chrono::duration<double, std::milli>(readEnd - readStart).count();
                    if (profileIO_) {
                        lastProfile_ = IOProfile{};
                        lastProfile_.panel_hit = true;
                        lastProfile_.pread_calls = 1;
                        lastProfile_.bytes_read = planeBytes;
                        lastProfile_.output_bytes = planeBytes;
                        lastProfile_.superblocks_touched = 1;
                        lastProfile_.plan_time_ms = planMs - readMs;
                        lastProfile_.read_time_ms = readMs;
                    }
                    return true;
                }
            }
        }
        // Fall through to SB if plane miss
    }

    // Try X-panel fast path
    if (axis == SliceAxis::X && hasXPanels(header_)) {
        adviseSequential(fd_);
        auto readStart = std::chrono::high_resolution_clock::now();
        bool panelOk = tryReadSliceXPanels(fd_, header_, index, output,
                                           profileIO_ ? &lastProfile_ : nullptr);
        auto readEnd = std::chrono::high_resolution_clock::now();
        if (panelOk) {
            auto planEnd = std::chrono::high_resolution_clock::now();
            double planMs = std::chrono::duration<double, std::milli>(planEnd - planStart).count();
            double readMs = std::chrono::duration<double, std::milli>(readEnd - readStart).count();
            lastProfile_.plan_time_ms = planMs - readMs;
            lastProfile_.read_time_ms = readMs;
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
    if (compressed_) {
        // Compressed path: sort tasks by physical file offset for HDD
        const uint64_t sbBV = getSuperblockBytes(header_);
        std::vector<uint8_t> sbBuf(sbBV);
        adviseSequential(fd_);

        std::vector<size_t> taskOrder(plan.tasks.size());
        for (size_t i = 0; i < plan.tasks.size(); ++i) taskOrder[i] = i;
        std::sort(taskOrder.begin(), taskOrder.end(), [&](size_t a, size_t b) {
            uint64_t sbIdxA = (plan.tasks[a].file_offset - header_.data_offset) / sbBV;
            uint64_t sbIdxB = (plan.tasks[b].file_offset - header_.data_offset) / sbBV;
            if (sbIdxA < compIndex_.size() && sbIdxB < compIndex_.size())
                return compIndex_[sbIdxA].file_offset < compIndex_[sbIdxB].file_offset;
            return plan.tasks[a].file_offset < plan.tasks[b].file_offset;
        });

        ok = true;
        uint64_t lastSbIdx = UINT64_MAX;
        for (size_t ti = 0; ti < taskOrder.size(); ++ti) {
            const auto& task = plan.tasks[taskOrder[ti]];
            uint64_t sbIdx = (task.file_offset - header_.data_offset) / sbBV;
            if (sbIdx != lastSbIdx) {
                if (!readSuperblock(sbIdx, sbBuf.data())) { ok = false; break; }
                lastSbIdx = sbIdx;
            }
            unpackLeaves(header_, plan, task, sbBuf.data(), output);
        }
    } else if (sbReadMode_ == SBReadMode::HDDReadWindow) {
        ok = executeSBPlanHDDReadWindow(fd_, plan, header_, output, 1,
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
    } else {
        // 默认使用 HDDReadWindow 模式
        ok = executeSBPlanHDDReadWindow(fd_, plan, header_, output, 1,
                                        memoryLimitMB, hddReadWindowCfg_,
                                        profileIO_ ? &lastProfile_ : nullptr,
                                        pinThreads_);
    }

    return ok;
}

bool ERWT3DReader::readSlicesBatch(const std::vector<SliceBatchRequest>& requests,
                                    int numThreads, size_t memoryLimitMB,
                                    const HDDReadWindowConfig& wcfg) {
    if (fd_ < 0 || requests.empty()) return false;

    // Step 0: Try raw X auxiliary batch path for all X requests at once
    std::vector<bool> rawXAuxHandled(requests.size(), false);
    if (rawXAuxAvailable_) {
        tryReadBatchRawXAux_(requests, rawXAuxHandled);
    }

    // Step 1: Try batch sidecar read for remaining X requests
    std::vector<bool> xpHandled(requests.size(), false);
    if (xpAvailable_) {
        tryReadBatchXPSidecar_(requests, xpHandled);
    }

    // Step 2: Process remaining requests individually
    std::vector<SBTaskPlan> plans;
    std::vector<float*> outputs;
    std::vector<const SBTaskPlan*> pp;
    std::vector<size_t> batchIdx;

    for (size_t i = 0; i < requests.size(); ++i) {
        const auto& r = requests[i];

        if (rawXAuxHandled[i] || xpHandled[i]) continue;

        // Try X-plane fast path
        if (r.axis == SliceAxis::X && hasXPlanes(header_)) {
            uint32_t stride = getXPlaneStride(header_);
            if (r.index % stride == 0) {
                uint64_t planeIdx = r.index / stride;
                uint64_t planeCount = getXPlaneCount(header_);
                if (planeIdx < planeCount) {
                    uint64_t planeBytes = header_.ny * header_.nz * sizeof(float);
                    uint64_t off = getXPlaneOffset(header_) + planeIdx * planeBytes;
                    xPlaneRawBuf_.resize(header_.ny * header_.nz);
                    ssize_t n = pread(fd_, xPlaneRawBuf_.data(), planeBytes, off);
                    if (n == static_cast<ssize_t>(planeBytes)) {
                        transposeZYToYZ(xPlaneRawBuf_.data(), r.output, header_.nz, header_.ny, header_.nz);
                        continue;
                    }
                }
            }
        }
        // Try X-panel fast path
        if (r.axis == SliceAxis::X && hasXPanels(header_)) {
            if (tryReadSliceXPanels(fd_, header_, r.index, r.output, nullptr))
                continue;
        }
        // Fall through to batch path
        SBTaskPlan p;
        switch (r.axis) {
            case SliceAxis::Z: p = buildSBPlanZ(header_, r.index); break;
            case SliceAxis::Y: p = buildSBPlanY(header_, r.index); break;
            case SliceAxis::X: p = buildSBPlanX(header_, r.index); break;
        }
        if (sbTaskOrder_ == SBTaskOrder::FileOffset) sortTasksByFileOffset(p);
        plans.push_back(std::move(p));
        outputs.push_back(r.output);
        batchIdx.push_back(i);
    }

    if (plans.empty()) return true;
    for (auto& p : plans) pp.push_back(&p);

    if (compressed_) {
        // Merged-window parallel LZ4 reader
        auto batch = buildSBBatchPlan(pp);
        const uint64_t sbBV = getSuperblockBytes(header_);
        const uint64_t totalSBs = compIndex_.size();

        // Collect unique SB tasks with scatter targets
        struct CompressedSBTask {
            uint64_t sb_idx;
            uint64_t compressed_offset;
            uint32_t compressed_size;
            uint8_t is_compressed;
            std::vector<size_t> scatter_indices; // into batch.batch_tasks
        };

        std::unordered_map<uint64_t, CompressedSBTask> sbMap;
        sbMap.reserve(batch.batch_tasks.size());

        for (size_t ti = 0; ti < batch.batch_tasks.size(); ++ti) {
            const auto& bt = batch.batch_tasks[ti];
            uint64_t sbIdx = (bt.file_offset - header_.data_offset) / sbBV;
            if (sbIdx >= totalSBs) continue;

            auto& entry = sbMap[sbIdx];
            if (entry.scatter_indices.empty()) {
                entry.sb_idx = sbIdx;
                entry.compressed_offset = compIndex_[sbIdx].file_offset;
                entry.compressed_size = compIndex_[sbIdx].compressed_size;
                entry.is_compressed = compIndex_[sbIdx].is_compressed;
            }
            entry.scatter_indices.push_back(ti);
        }

        if (sbMap.empty()) return true;

        // Sort by compressed file offset
        std::vector<CompressedSBTask*> sortedSBs;
        sortedSBs.reserve(sbMap.size());
        for (auto& kv : sbMap) sortedSBs.push_back(&kv.second);
        std::sort(sortedSBs.begin(), sortedSBs.end(),
                  [](const CompressedSBTask* a, const CompressedSBTask* b) {
                      return a->compressed_offset < b->compressed_offset;
                  });

        const uint64_t WINDOW_SIZE = 256ULL * 1024 * 1024;
        const uint64_t MAX_GAP = 4ULL * 1024 * 1024;
        const int decodeThreads = std::min(8, std::max(1, numThreads));

        ThreadPool pool(static_cast<size_t>(decodeThreads));
        std::vector<std::vector<uint8_t>> workerBufs(decodeThreads);
        for (auto& wb : workerBufs) wb.resize(sbBV);

        std::vector<uint8_t> windowBuf;

        adviseSequential(fd_);

        size_t si = 0;
        while (si < sortedSBs.size()) {
            // Build window
            uint64_t wstart = sortedSBs[si]->compressed_offset;
            uint64_t wend = wstart + sortedSBs[si]->compressed_size;
            size_t sj = si + 1;
            while (sj < sortedSBs.size()) {
                uint64_t next_off = sortedSBs[sj]->compressed_offset;
                if (next_off > wend + MAX_GAP) break;
                uint64_t next_end = next_off + sortedSBs[sj]->compressed_size;
                if (next_end - wstart > WINDOW_SIZE) break;
                wend = next_end;
                ++sj;
            }

            uint64_t wsize = wend - wstart;
            if (windowBuf.size() < wsize)
                windowBuf.resize(wsize);

            // Prefetch next window
            if (sj < sortedSBs.size()) {
                uint64_t next_start = sortedSBs[sj]->compressed_offset;
                readahead(fd_, static_cast<off_t>(next_start),
                          static_cast<size_t>(128ULL * 1024 * 1024));
            }

            // Read window
            ssize_t n = pread(fd_, windowBuf.data(), wsize, wstart);
            if (n != static_cast<ssize_t>(wsize)) return false;

            // Parallel decompress + scatter
            size_t count = sj - si;
            if (count <= 1 || decodeThreads <= 1) {
                for (size_t k = si; k < sj; ++k) {
                    auto* sb = sortedSBs[k];
                    uint8_t* decompBuf = workerBufs[0].data();
                    const uint8_t* srcData = windowBuf.data() +
                        (sb->compressed_offset - wstart);

                    if (sb->is_compressed) {
#ifdef ERWT3D_HAVE_LZ4
                        int dec = LZ4_decompress_safe(
                            reinterpret_cast<const char*>(srcData),
                            reinterpret_cast<char*>(decompBuf),
                            static_cast<int>(sb->compressed_size),
                            static_cast<int>(sbBV));
                        if (dec != static_cast<int>(sbBV)) return false;
#else
                        return false;
#endif
                    } else {
                        std::memcpy(decompBuf, srcData, sbBV);
                    }

                    for (size_t si2 : sb->scatter_indices) {
                        const auto& bt = batch.batch_tasks[si2];
                        SBTask t{bt.file_offset, bt.first_leaf, bt.leaf_count};
                        unpackLeaves(header_, *bt.plan, t, decompBuf,
                                     outputs[bt.output_id]);
                    }
                }
            } else {
                std::vector<std::future<bool>> futures;
                const size_t per = (count + decodeThreads - 1) / decodeThreads;
                for (int t = 0; t < decodeThreads; ++t) {
                    const size_t kstart = si + static_cast<size_t>(t) * per;
                    const size_t kend = std::min(kstart + per, sj);
                    if (kstart >= kend) break;
                    futures.push_back(pool.submit([&, kstart, kend, t]() -> bool {
                        uint8_t* decompBuf = workerBufs[t].data();
                        for (size_t k = kstart; k < kend; ++k) {
                            auto* sb = sortedSBs[k];
                            const uint8_t* srcData = windowBuf.data() +
                                (sb->compressed_offset - wstart);

                            if (sb->is_compressed) {
#ifdef ERWT3D_HAVE_LZ4
                                int dec = LZ4_decompress_safe(
                                    reinterpret_cast<const char*>(srcData),
                                    reinterpret_cast<char*>(decompBuf),
                                    static_cast<int>(sb->compressed_size),
                                    static_cast<int>(sbBV));
                                if (dec != static_cast<int>(sbBV)) return false;
#else
                                return false;
#endif
                            } else {
                                std::memcpy(decompBuf, srcData, sbBV);
                            }

                            for (size_t si2 : sb->scatter_indices) {
                                const auto& bt = batch.batch_tasks[si2];
                                SBTask t{bt.file_offset, bt.first_leaf, bt.leaf_count};
                                unpackLeaves(header_, *bt.plan, t, decompBuf,
                                             outputs[bt.output_id]);
                            }
                        }
                        return true;
                    }));
                }
                for (auto& f : futures) {
                    if (!f.get()) return false;
                }
            }

            si = sj;
        }
        return true;
    }

    return executeSBBatchHDD(fd_, buildSBBatchPlan(pp), header_, outputs.data(),
                              numThreads, memoryLimitMB, wcfg, pinThreads_, nullptr);
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

bool ERWT3DReader::readLineBatched(const std::vector<Extent>& extents,
                                    const std::vector<uint64_t>& extentBases,
                                    uint64_t srcOff, uint64_t srcStride,
                                    uint64_t dimLen, uint64_t elemStride,
                                    float* output, int numThreads, size_t memoryLimitMB) {
    auto merged = mergeExtents(extents);
    size_t maxBuf = memoryLimitMB * 1024 * 1024;
    if (maxBuf == 0) maxBuf = 1024 * 1024;
    size_t totalSize = 0; for (auto& e : merged) totalSize += e.size;

    if (totalSize <= maxBuf) {
        std::vector<uint8_t> buf(totalSize);
        if (numThreads <= 1) { if (!readExtents(merged, buf.data())) return false; }
        else { if (!readExtentsThreaded(merged, buf.data(), numThreads)) return false; }
        for (size_t i = 0; i < extents.size(); ++i) {
            uint64_t bo = 0; for (size_t j = 0; j < merged.size(); ++j) {
                if (extents[i].offset >= merged[j].offset && extents[i].offset < merged[j].end()) {
                    const float* ld = reinterpret_cast<const float*>(buf.data() + bo + (extents[i].offset - merged[j].offset));
                    uint64_t base = extentBases[i], valid = std::min(elemStride, dimLen - base);
                    for (uint64_t d = 0; d < valid; ++d)
                        output[base + d] = ld[d * srcStride + srcOff];
                    break;
                }
                bo += merged[j].size;
            }
        }
        return true;
    }

    uint64_t batchSz = 0; size_t bs = 0;
    for (size_t i = 0; i <= merged.size(); ++i) {
        bool flush = (i == merged.size());
        if (!flush) { uint64_t nxt = batchSz + merged[i].size; if (nxt <= maxBuf) { batchSz = nxt; continue; } if (batchSz == 0) { batchSz = merged[i].size; continue; } flush = true; }
        if (bs < i) {
            size_t be = i; uint64_t bsz = 0;
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
                        uint64_t base = extentBases[k], valid = std::min(elemStride, dimLen - base);
                        for (uint64_t d = 0; d < valid; ++d)
                            output[base + d] = ld[d * srcStride + srcOff];
                    }
                }
                bo2 += merged[j].size;
            }
        }
        bs = i; batchSz = 0; if (!flush) batchSz = merged[i].size;
    }
    return true;
}

void ERWT3DReader::setHDDMode() {
    ioBackend_ = IOBackend::Superblock;
    sbReadMode_ = SBReadMode::HDDReadWindow;
    sbTaskOrder_ = SBTaskOrder::FileOffset;
    hddReadWindowCfg_ = HDDReadWindowConfig{128 * 1024 * 1024, 3 * 1024 * 1024};
}

} // namespace erwt3d
