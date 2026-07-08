#pragma once

#include "tri_format.hpp"
#include "sb_plan.hpp"
#include "sb_hdd.hpp"
#include "thread_pool.hpp"
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <mutex>

namespace erwt3d {

enum class TriSliceAxis { X = 0, Y = 1, Z = 2 };

struct TriProfile {
    double read_time_ms = 0;
    double decode_time_ms = 0;
    double scatter_time_ms = 0;
    uint64_t pread_calls = 0;
    uint64_t bytes_read = 0;
    uint64_t blocks_decoded = 0;
    uint64_t exception_blocks = 0;
    uint64_t exception_bytes_read = 0;
    double total_ms = 0;
};

struct TriSliceBatchRequest {
    TriSliceAxis axis;
    uint64_t index;
    float* output;
};

class TriReader {
public:
    TriReader(const std::string& path, size_t cacheMB = 0);
    ~TriReader();

    const TriHeader& getHeader() const { return header_; }

    bool readSlice(TriSliceAxis axis, uint64_t index, float* output);
    bool readSlicesBatch(const std::vector<TriSliceBatchRequest>& reqs,
                         int numThreads, size_t memoryLimitMB,
                         const HDDReadWindowConfig& wcfg);

    void setProfileIO(bool enable) { profileIO_ = enable; }
    const TriProfile& lastProfile() const { return lastProfile_; }

    void setHDDMode();
    bool hddMode() const { return hddMode_; }
    void setNumThreads(int n) { numThreads_ = n; }

    uint64_t slabOffsetFor(TriSliceAxis axis, uint64_t index) const {
        return axisBlockOffset(axis, index / 4);
    }
    uint64_t slabBytesFor(TriSliceAxis axis) const {
        return axisBlockCount(axis) * blockBytes_;
    }
    void prefetchSlab(TriSliceAxis axis, uint64_t index);

private:
    std::string path_;
    TriHeader header_;
    int fd_;
    bool profileIO_ = false;
    bool hddMode_ = false;
    int numThreads_ = 1;
    TriProfile lastProfile_;

    // ZFP state (per-thread via thread_local in impl)
    bool hasZfp_ = false;
    double rate_ = 0;
    uint64_t blockBytes_ = 0;

    // Exception state
    bool hasException_ = false;
    std::vector<TriExceptionIndex> excIndex_;
    std::mutex excMutex_;

    uint64_t bxCount_ = 0, byCount_ = 0, bzCount_ = 0;

    // Reusable buffers
    std::vector<uint8_t> slabBuf_;
    std::unique_ptr<ThreadPool> pool_;

    uint64_t axisBlockOffset(TriSliceAxis axis, uint64_t outerIdx) const;
    uint64_t axisBlockCount(TriSliceAxis axis) const;

    bool readSliceZfp(TriSliceAxis axis, uint64_t index, float* output,
                      TriProfile* profile);
    bool readSliceRaw(TriSliceAxis axis, uint64_t index, float* output,
                      TriProfile* profile);

    void loadExceptionIndex();
    bool isExceptionBlock(uint64_t bx, uint64_t by, uint64_t bz) const;
    bool readExceptionBlock(uint64_t bx, uint64_t by, uint64_t bz, float* blockOut);
};

} // namespace erwt3d
