#pragma once

#include "sb_plan.hpp"

namespace erwt3d {

// ========== HDD-Optimized Execution ==========
//
// HDD特点: 顺序读快，随机读慢(寻道~10ms)，需要最小化寻道次数
// 策略: 合并连续读、大读窗口、按偏移排序

// RunBatch: 合并连续superblock为单次pread (减少syscall)
bool executeSBPlanRunBatch(int fd, const SBTaskPlan& plan, const ERWT3DHeader& hdr,
                            float* output, int numThreads, size_t memoryLimitMB,
                            IOProfile* profile, bool pinThreads = false);

// LeafIndex: 只读取需要的leaf blocks，按偏移排序合并 (最小化读取量)
bool executeSBPlanLeafIndex(int fd, const SBTaskPlan& plan, const ERWT3DHeader& hdr,
                             float* output, int numThreads, size_t memoryLimitMB,
                             size_t leafMergeBytes, IOProfile* profile, bool pinThreads = false);

// ========== HDD Read Window ==========

struct HDDReadWindowConfig {
    uint64_t read_window_bytes = 0;  // 0 = auto (512 MiB)
    uint64_t max_gap_bytes = 0;      // 0 = auto (8 MiB)

    // Drive characteristics used by the automatic read-strategy selector.
    // A value of 0 means "use a built-in default".
    double seek_ms = 0.0;            // default 9 ms
    double sequential_mb_s = 0.0;    // default 220 MB/s
};

// HDDReadWindow: 大连续读窗口 + gap容忍 (HDD最优)
bool executeSBPlanHDDReadWindow(int fd, const SBTaskPlan& plan, const ERWT3DHeader& hdr,
                                 float* output, int numThreads, size_t memoryLimitMB,
                                 const HDDReadWindowConfig& cfg, IOProfile* profile,
                                 bool pinThreads = false);

// ========== Batch HDD (多切片合并) ==========

struct HDDContiguousConfig {
    uint32_t prefetch_slices = 0;
};

struct SBBatchTask {
    uint64_t file_offset;
    uint32_t first_leaf;
    uint32_t leaf_count;
    uint32_t output_id;
    const SBTaskPlan* plan;
};

struct SBBatchPlan {
    std::vector<const SBTaskPlan*> plans;
    std::vector<SBBatchTask> batch_tasks;
    uint64_t total_sb_touched = 0;
};

struct SBBatchProfile {
    uint64_t windows_count = 0;
    uint64_t pread_calls = 0;
    uint64_t bytes_actual_read = 0;
    uint64_t superblocks_decoded = 0;
};

// Build batch plan: global sort + merge across slice boundaries
SBBatchPlan buildSBBatchPlan(const std::vector<const SBTaskPlan*>& plans);

// Execute batch with HDD-optimized windowed reads
bool executeSBBatchHDD(int fd, const SBBatchPlan& batch, const ERWT3DHeader& hdr,
                        float* const* outputs, int numThreads, size_t memoryLimitMB,
                        const HDDReadWindowConfig& wcfg, bool pinThreads = false,
                        SBBatchProfile* profile = nullptr);

} // namespace erwt3d
