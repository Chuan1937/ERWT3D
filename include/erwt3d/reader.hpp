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
    
    // Get header info
    const ERWT3DHeader& getHeader() const { return header_; }
    
    // Read a slice
    bool readSlice(SliceAxis axis, uint64_t index, float* output);
    
    // Read a line along X axis
    bool readLineX(uint64_t y, uint64_t z, float* output);
    
    // Read entire volume to raw float32
    bool readFull(float* output, int numThreads = 1, size_t memoryLimitMB = 2048);
    
    // Read full volume to file
    bool readFullToFile(const std::string& outputPath, int numThreads = 1, size_t memoryLimitMB = 2048);

private:
    std::string path_;
    ERWT3DHeader header_;
    int fd_;
    std::unique_ptr<LeafCache> cache_;
    
    // Read extents from file
    bool readExtents(const std::vector<Extent>& extents, void* buffer);
};

} // namespace erwt3d