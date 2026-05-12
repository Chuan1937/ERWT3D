#pragma once

#include "format.hpp"
#include "slice.hpp"
#include "cache.hpp"
#include <cstdint>
#include <string>
#include <memory>

namespace erwt3d {

enum class IOBackend {
    PRead,       // per-merged-extent pread (default)
    Superblock,  // read whole superblocks, extract leaves
};

class ERWT3DReader {
public:
    ERWT3DReader(const std::string& path, size_t cacheMB = 0);
    ~ERWT3DReader();
    
    const ERWT3DHeader& getHeader() const { return header_; }
    
    bool readSlice(SliceAxis axis, uint64_t index, float* output);
    bool readSlice(SliceAxis axis, uint64_t index, float* output,
                   int numThreads, size_t memoryLimitMB);
    
    bool readLineX(uint64_t y, uint64_t z, float* output);
    bool readLineX(uint64_t y, uint64_t z, float* output,
                   int numThreads, size_t memoryLimitMB);
    
    bool readFull(float* output, int numThreads = 1, size_t memoryLimitMB = 2048);
    bool readFullToFile(const std::string& outputPath, int numThreads = 1, size_t memoryLimitMB = 2048);
    
    void setCacheMB(size_t cacheMB);
    size_t cacheMB() const { return cacheMB_; }
    
    void setIOBackend(IOBackend b) { ioBackend_ = b; }
    IOBackend ioBackend() const { return ioBackend_; }

private:
    std::string path_;
    ERWT3DHeader header_;
    int fd_;
    std::unique_ptr<LeafCache> cache_;
    size_t cacheMB_ = 0;
    IOBackend ioBackend_ = IOBackend::PRead;
    
    bool readExtents(const std::vector<Extent>& extents, void* buffer);
    bool readExtentsThreaded(const std::vector<Extent>& extents, void* buffer, int numThreads);
    bool readOneExtent(uint64_t offset, uint64_t size, void* buffer);
    
    bool readSlicePRead(SliceAxis axis, uint64_t index, float* output,
                        int numThreads, size_t memoryLimitMB);
    bool readSliceSB(SliceAxis axis, uint64_t index, float* output,
                     int numThreads, size_t memoryLimitMB);
};

} // namespace erwt3d