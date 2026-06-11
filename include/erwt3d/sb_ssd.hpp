#pragma once

#include "sb_plan.hpp"

namespace erwt3d {

// ========== SSD-Optimized Execution ==========
//
// SSD特点: 随机读性能好，IOPS高，不需要特别优化寻道
// 策略: 多线程并行读取，每个线程独立pread

// Serial: 单线程逐superblock读取 (baseline)
bool executeSBPlanSerial(int fd, const SBTaskPlan& plan, const ERWT3DHeader& hdr,
                         float* output, IOProfile* profile);

// ParallelRead: 多线程并行读取superblocks (SSD最优)
//   - Static schedule: 均匀分配任务
//   - Dynamic schedule: 原子计数器动态分配
bool executeSBPlanParallelRead(int fd, const SBTaskPlan& plan, const ERWT3DHeader& hdr,
                                float* output, int numThreads, IOProfile* profile,
                                SBSchedule schedule = SBSchedule::Static,
                                bool pinThreads = false);

} // namespace erwt3d
