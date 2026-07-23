#pragma once

#include "format.hpp"
#include "slice.hpp"
#include "cache.hpp"
#include "sb_plan.hpp"
#include "sb_hdd.hpp"
#include "sb_panel.hpp"
#include "ssd/ssd_config.hpp"
#include <cstdint>
#include <string>
#include <memory>
#include <vector>

namespace erwt3d {

enum class IOBackend {
    PRead,       // per-merged-extent pread (default)
    Superblock,  // read whole superblocks, extract leaves
};

enum class SBReadMode {
    RunBatch,               // batch contiguous superblock runs into single pread
    LeafIndex,              // read only needed leaf blocks, merge into extents
    HDDReadWindow,          // HDD-max: large contiguous read windows with configurable gap tolerance
    SSDConcurrentExtent,    // SSD: concurrent small-extent pread with decode pipeline
};

class ERWT3DReader {
public:
    ERWT3DReader(const std::string& path, size_t cacheMB = 0, bool useMmap = false);
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
    void setSSDReadConfig(const SSDReadConfig& cfg) { ssdReadCfg_ = cfg; }
    const SSDReadConfig& ssdReadConfig() const { return ssdReadCfg_; }
    struct SliceBatchRequest { SliceAxis axis; uint64_t index; float* output; };
    bool readSlicesBatch(const std::vector<SliceBatchRequest>& requests,
                         int numThreads, size_t memoryLimitMB,
                         const HDDReadWindowConfig& wcfg);

    // SSD-mode batch read
    bool readSlicesBatchSSD(const std::vector<SliceBatchRequest>& requests,
                            int numThreads, size_t memoryLimitMB);

    // 一键配置
    void setHDDMode();

void setProfileIO(bool enable) { profileIO_ = enable; }
    bool profileIO() const { return profileIO_; }
    const IOProfile& lastProfile() const { return lastProfile_; }

    bool rawXAuxAvailable() const { return rawXAuxAvailable_; }

private:
    std::string path_;
    ERWT3DHeader header_;
    int fd_;
    std::unique_ptr<LeafCache> cache_;
    size_t cacheMB_ = 0;
    bool useMmap_ = false;
    void* mmapData_ = nullptr;
    size_t mmapSize_ = 0;
    IOBackend ioBackend_ = IOBackend::PRead;
    bool pinThreads_ = false;
    SBReadMode sbReadMode_ = SBReadMode::HDDReadWindow;
    size_t leafMergeBytes_ = 16384;
    SBTaskOrder sbTaskOrder_ = SBTaskOrder::FileOffset;
    HDDReadWindowConfig hddReadWindowCfg_;
    HDDContiguousConfig hddContigCfg_;
    SSDReadConfig ssdReadCfg_;
    bool profileIO_ = false;
    IOProfile lastProfile_;
    bool compressed_ = false;
    std::vector<CompressedBlockIndex> compIndex_;
    
    int xpFd_ = -1;
    XPSidecarHeader xpHeader_{};
    std::vector<XPChunkIndex> xpIndex_;
    bool xpAvailable_ = false;
    std::vector<uint8_t> xpCompBuf_;
    std::vector<uint8_t> xpRawBuf_;
    std::vector<float> xPlaneRawBuf_;
    void loadSidecar_();
    bool tryReadSliceXPSidecar_(uint64_t x, float* output, IOProfile* profile);
    bool tryReadBatchXPSidecar_(const std::vector<SliceBatchRequest>& requests,
                                std::vector<bool>& handled);
    
    // Raw X auxiliary (full-coverage uncompressed X-plane region)
    int rawXAuxFd_ = -1;
    bool rawXAuxAvailable_ = false;
    uint64_t rawXAuxOffset_ = 0;
    uint64_t rawXAuxBytes_ = 0;
    uint64_t rawXAuxPlaneBytes_ = 0;
    std::vector<uint8_t> rawXAuxWindowBuf_;
    void initRawXAux_();
    bool tryReadSliceRawXAux_(uint64_t x, float* output);
    bool tryReadBatchRawXAux_(const std::vector<SliceBatchRequest>& requests,
                              std::vector<bool>& handled);
    
    bool readExtents(const std::vector<Extent>& extents, void* buffer);
    bool readExtentsThreaded(const std::vector<Extent>& extents, void* buffer, int numThreads);
    bool readOneExtent(uint64_t offset, uint64_t size, void* buffer);
    bool readSuperblock(uint64_t sbIdx, void* buffer);
    
    bool readSliceSB(SliceAxis axis, uint64_t index, float* output,
                      int numThreads, size_t memoryLimitMB);

    bool readLineBatched(const std::vector<Extent>& extents,
                         const std::vector<uint64_t>& extentBases,
                         uint64_t srcOff, uint64_t srcStride,
                         uint64_t dimLen, uint64_t elemStride,
                         float* output, int numThreads, size_t memoryLimitMB);
};

} // namespace erwt3d
