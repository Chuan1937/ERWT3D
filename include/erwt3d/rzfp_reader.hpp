#pragma once

#include "rzfp_format.hpp"
#include "rzfp_xplane_codec.hpp"
#include "slice.hpp"
#include "sb_hdd.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace erwt3d {

enum class RzfpReadStrategy {
    Auto = 0,
    SelectiveLeaf,
    WholeSuperblock,
    FullPayloadScan,
};

struct RzfpReadProfile {
    uint64_t unique_superblocks = 0;
    uint64_t unique_leaves = 0;
    uint64_t requested_record_bytes = 0;
    uint64_t actual_read_bytes = 0;
    uint64_t pread_calls = 0;
    double plan_time_ms = 0.0;
    double prefix_time_ms = 0.0;
    double io_time_ms = 0.0;
    double decode_time_ms = 0.0;
    double scatter_time_ms = 0.0;
    RzfpReadStrategy selected_strategy = RzfpReadStrategy::Auto;

    double readAmplification() const {
        return requested_record_bytes > 0
                   ? static_cast<double>(actual_read_bytes) / static_cast<double>(requested_record_bytes)
                   : 1.0;
    }
};

struct RzfpReaderConfig {
    HDDReadWindowConfig hdd;
    RzfpReadStrategy strategy = RzfpReadStrategy::Auto;
    int decode_threads = 1;
    RzfpReadProfile* profile = nullptr;
};

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

    bool readSlicesBatch(const std::vector<SliceBatchRequest>& requests,
                         const RzfpReaderConfig& config);

private:
    std::string path_;
    int fd_ = -1;
    RzfpFileHeader header_{};

    std::vector<RzfpSuperblockIndex> sb_index_;
    std::vector<RzfpLeafDescriptor> descriptors_;

    // Optional 2D X-plane sidecar.
    bool has_xplane_ = false;
    int xplane_fd_ = -1;
    std::vector<uint64_t> xplane_offsets_;
    std::vector<uint32_t> xplane_sizes_;

    bool openXPlaneSidecar();
    bool readXPlaneFromSidecar(uint64_t x, float* output, RzfpReadProfile* profile);
    bool readXPlanesBatchFromSidecar(
        const std::vector<SliceBatchRequest>& requests,
        const RzfpReaderConfig& config,
        RzfpReadProfile& profile
    );
};

} // namespace erwt3d
