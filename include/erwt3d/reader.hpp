#pragma once

#include "format.hpp"
#include "slice.hpp"
#include "cache.hpp"
#include <cstdint>
#include <string>
#include <memory>

namespace erwt3d {

class ERWT3DReader {
public:
    ERWT3DReader(const std::string& path, size_t cacheMB = 0);
    ~ERWT3DReader();
    
    const ERWT3DHeader& getHeader() const { return header_; }
    
    // Read a slice (default: single-threaded, legacy)
    bool readSlice(SliceAxis axis, uint64_t index, float* output);
    
    // Read a slice with thread and memory control
    bool readSlice(SliceAxis axis, uint64_t index, float* output,
                   int numThreads, size_t memoryLimitMB);
    
    // Read a line along X axis (default)
    bool readLineX(uint64_t y, uint64_t z, float* output);
    
    // Read a line along X axis with thread and memory control
    bool readLineX(uint64_t y, uint64_t z, float* output,
                   int numThreads, size_t memoryLimitMB);
    
    // Read entire volume to raw float32 (for tests/small volumes)
    bool readFull(float* output, int numThreads = 1, size_t memoryLimitMB = 2048);
    
    // Read full volume to file (streaming, for large volumes)
    bool readFullToFile(const std::string& outputPath, int numThreads = 1, size_t memoryLimitMB = 2048);
    
    // Set cache size at runtime
    void setCacheMB(size_t cacheMB);
    
    // Get current cache size
    size_t cacheMB() const { return cacheMB_; }

private:
    std::string path_;
    ERWT3DHeader header_;
    int fd_;
    std::unique_ptr<LeafCache> cache_;
    size_t cacheMB_;
    
    bool readExtents(const std::vector<Extent>& extents, void* buffer);
    bool readExtentsThreaded(const std::vector<Extent>& extents, void* buffer, int numThreads);
    
    // Cache-aware single extent read
    bool readOneExtent(uint64_t offset, uint64_t size, void* buffer);
};

} // namespace erwt3d