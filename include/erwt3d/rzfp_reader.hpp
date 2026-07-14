#pragma once

#include "rzfp_format.hpp"
#include "slice.hpp"
#include "sb_hdd.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace erwt3d {

class RzfpReader {
public:
    explicit RzfpReader(const std::string& path);
    ~RzfpReader();

    bool ok() const { return fd_ >= 0; }
    const RzfpFileHeader& header() const { return header_; }

    bool readSlice(SliceAxis axis, uint64_t index, float* output,
                   int numThreads = 1, size_t memoryLimitMB = 4096,
                   const HDDReadWindowConfig& wcfg = {});

    struct SliceBatchRequest {
        SliceAxis axis;
        uint64_t index;
        float* output;
    };

    bool readSlicesBatch(const std::vector<SliceBatchRequest>& requests,
                         int numThreads = 1, size_t memoryLimitMB = 4096,
                         const HDDReadWindowConfig& wcfg = {});

private:
    std::string path_;
    int fd_ = -1;
    RzfpFileHeader header_{};

    std::vector<RzfpSuperblockIndex> sb_index_;
    std::vector<RzfpLeafDescriptor> descriptors_;
};

} // namespace erwt3d
