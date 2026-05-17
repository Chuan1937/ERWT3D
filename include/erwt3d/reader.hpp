#pragma once

#include "format.hpp"
#include "slice.hpp"
#include "cache.hpp"
#include "sb_task.hpp"
#include <cstdint>
#include <string>
#include <memory>

namespace erwt3d {

enum class IOBackend {
    PRead,       // per-merged-extent pread (default)
    Superblock,  // read whole superblocks, extract leaves
};

enum class SBParallelMode {
    Serial,       // current stable single-thread SB path (default)
    ParallelRead, // parallel pread over superblocks, disjoint output regions
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
    
    bool readLine(SliceAxis axis, uint64_t fixed1, uint64_t fixed2, float* output,
                  int numThreads = 1, size_t memoryLimitMB = 2048);
    bool readLineY(uint64_t x, uint64_t z, float* output,
                   int numThreads = 1, size_t memoryLimitMB = 2048);
    bool readLineZ(uint64_t x, uint64_t y, float* output,
                   int numThreads = 1, size_t memoryLimitMB = 2048);
    
    bool readFull(float* output, int numThreads = 1, size_t memoryLimitMB = 2048);
    bool readFullToFile(const std::string& outputPath, int numThreads = 1, size_t memoryLimitMB = 2048);
    
    void setCacheMB(size_t cacheMB);
    size_t cacheMB() const { return cacheMB_; }
    
    void setIOBackend(IOBackend b) { ioBackend_ = b; }
    IOBackend ioBackend() const { return ioBackend_; }
    
    void setSBParallelMode(SBParallelMode m) { sbParallelMode_ = m; }
    SBParallelMode sbParallelMode() const { return sbParallelMode_; }
    
    void setSBSchedule(SBSchedule s) { sbSchedule_ = s; }
    SBSchedule sbSchedule() const { return sbSchedule_; }
    
    void setPinThreads(bool enable) { pinThreads_ = enable; }
    bool pinThreads() const { return pinThreads_; }
    
    void setSBReadMode(SBReadMode m) { sbReadMode_ = m; }
    SBReadMode sbReadMode() const { return sbReadMode_; }
    
    void setLeafMergeBytes(size_t bytes) { leafMergeBytes_ = bytes; }
    size_t leafMergeBytes() const { return leafMergeBytes_; }
    
    void setSBTaskOrder(SBTaskOrder o) { sbTaskOrder_ = o; }
    SBTaskOrder sbTaskOrder() const { return sbTaskOrder_; }
    
    void setHDDReadWindowConfig(const HDDReadWindowConfig& cfg) { hddReadWindowCfg_ = cfg; }
    const HDDReadWindowConfig& hddReadWindowConfig() const { return hddReadWindowCfg_; }
    void setHDDContiguousConfig(const HDDContiguousConfig& c) { hddContigCfg_ = c; }
    const HDDContiguousConfig& hddContiguousConfig() const { return hddContigCfg_; }
    struct SliceBatchRequest { SliceAxis axis; uint64_t index; float* output; };
    bool readSlicesBatch(const std::vector<SliceBatchRequest>& requests,
                         int numThreads, size_t memoryLimitMB,
                         const HDDReadWindowConfig& wcfg,
                         SBBatchProfile* profile = nullptr);
    void setProfileIO(bool enable) { profileIO_ = enable; }
    bool profileIO() const { return profileIO_; }
    const IOProfile& lastProfile() const { return lastProfile_; }

private:
    std::string path_;
    ERWT3DHeader header_;
    int fd_;
    std::unique_ptr<LeafCache> cache_;
    size_t cacheMB_ = 0;
    IOBackend ioBackend_ = IOBackend::PRead;
    SBParallelMode sbParallelMode_ = SBParallelMode::Serial;
    SBSchedule sbSchedule_ = SBSchedule::Static;
    bool pinThreads_ = false;
    SBReadMode sbReadMode_ = SBReadMode::PRead;
    size_t leafMergeBytes_ = 16384;
    SBTaskOrder sbTaskOrder_ = SBTaskOrder::Logical;
    HDDReadWindowConfig hddReadWindowCfg_;
    HDDContiguousConfig hddContigCfg_;
    std::vector<uint64_t> tileDir_;
    bool profileIO_ = false;
    IOProfile lastProfile_;
    
    bool readExtents(const std::vector<Extent>& extents, void* buffer);
    bool readExtentsThreaded(const std::vector<Extent>& extents, void* buffer, int numThreads);
    bool readOneExtent(uint64_t offset, uint64_t size, void* buffer);
    
    bool readSlicePRead(SliceAxis axis, uint64_t index, float* output,
                        int numThreads, size_t memoryLimitMB);
    bool readSliceSB(SliceAxis axis, uint64_t index, float* output,
                      int numThreads, size_t memoryLimitMB);
};

} // namespace erwt3d
