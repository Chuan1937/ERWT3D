// sb_hdd.cpp: HDD-optimized I/O execution
//
// HDD特点: 顺序读快，随机读慢(寻道~10ms)
// 策略: 最小化寻道次数，合并连续读，大读窗口
//
// 包含:
//   - executeSBPlanRunBatch: 合并连续superblock为单次pread
//   - executeSBPlanLeafIndex: 只读取需要的leaf blocks，按偏移排序合并
//   - executeSBPlanHDDReadWindow: 大连续读窗口 + gap容忍
//   - executeSBBatchHDD: 多切片批量读取
//
// HDD优化技术:
//   - posix_fadvise: 提示内核访问模式，优化readahead
//   - readahead: 显式预取顺序数据
//   - 大读窗口: 减少寻道次数
//   - gap容忍: 允许小gap合并为大读

#include "erwt3d/sb_hdd.hpp"
#include "erwt3d/thread_pool.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <random>
#include <unistd.h>
#include <fcntl.h>

namespace erwt3d {

using namespace detail;

namespace {

inline void adviseSequential(int fd) {
#if defined(POSIX_FADV_SEQUENTIAL)
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#else
    (void)fd;
#endif
}

inline void tryReadahead(int fd, uint64_t offset, uint64_t bytes) {
#if defined(__linux__)
    readahead(fd, static_cast<off_t>(offset), static_cast<size_t>(bytes));
#else
    (void)fd;
    (void)offset;
    (void)bytes;
#endif
}

struct Window {
    uint64_t file_offset;
    uint64_t read_bytes;
    size_t first_task;
    size_t task_count;
};

std::vector<Window> buildWindows(const uint64_t* offsets, size_t n,
                                  uint64_t sbBV, uint64_t rwBytes, uint64_t gapBytes) {
    std::vector<Window> windows;
    for (size_t i = 0; i < n; ) {
        Window w;
        w.file_offset = offsets[i];
        w.first_task = i;
        w.task_count = 1;
        w.read_bytes = sbBV;
        while (i + w.task_count < n) {
            uint64_t nextOff = offsets[i + w.task_count];
            uint64_t curEnd = w.file_offset + w.read_bytes;
            if (nextOff < curEnd) { ++w.task_count; continue; }
            uint64_t gap = nextOff - curEnd;
            uint64_t extended = curEnd + gap + sbBV - w.file_offset;
            if (gap == 0 && extended <= rwBytes) {
                w.read_bytes += sbBV;
                ++w.task_count;
            } else if (gap > 0 && gap <= gapBytes && extended <= rwBytes) {
                w.read_bytes += gap + sbBV;
                ++w.task_count;
            } else {
                break;
            }
        }
        windows.push_back(w);
        i += w.task_count;
    }
    return windows;
}

inline void prefetchWindows(int fd, const std::vector<Window>& wins,
                             size_t curIdx, size_t endIdx, int count) {
    for (int ahead = 1; ahead <= count && curIdx + ahead < endIdx; ++ahead) {
        const auto& fw = wins[curIdx + ahead];
        if (fw.file_offset > wins[curIdx].file_offset)
            tryReadahead(fd, fw.file_offset, fw.read_bytes);
    }
}

} // anonymous namespace

// ============================================================================
// RunBatch: Merge contiguous superblock reads
//
// 将连续的superblock合并为单次pread，减少syscall次数
// 适用于: superblocks在文件中连续存储的场景
// ============================================================================

bool executeSBPlanRunBatch(int fd, const SBTaskPlan& plan, const ERWT3DHeader& hdr,
                            float* output, int numThreads, size_t memoryLimitMB,
                            IOProfile* profile, bool pinThreads) {
    const uint64_t sbBV = sbBytes(hdr);
    size_t n = plan.tasks.size();
    if (n == 0) return true;
    if (numThreads <= 1) numThreads = 1;

    // HDD优化: 提示内核顺序访问模式
    adviseSequential(fd);

    // Build runs: group contiguous superblock reads
    struct Run { uint64_t file_offset; uint64_t bytes; size_t first_task; size_t task_count; };
    std::vector<Run> runs;
    for (size_t i = 0; i < n; ) {
        Run r;
        r.file_offset = plan.tasks[i].file_offset;
        r.first_task = i;
        r.task_count = 1;
        r.bytes = sbBV;
        while (i + r.task_count < n &&
               plan.tasks[i + r.task_count].file_offset == r.file_offset + r.bytes) {
            r.bytes += sbBV;
            ++r.task_count;
        }
        runs.push_back(r);
        i += r.task_count;
    }

    size_t maxRunBytes = 0;
    for (const auto& r : runs) maxRunBytes = std::max<size_t>(maxRunBytes, static_cast<size_t>(r.bytes));
    size_t maxBufPerThread = memoryLimitMB * 1024ULL * 1024ULL / static_cast<size_t>(numThreads);
    if (maxBufPerThread < sbBV) return false; // memory limit too small for one superblock
    maxBufPerThread = std::min(maxBufPerThread, std::max<size_t>(maxRunBytes, static_cast<size_t>(sbBV * 4)));

    auto processRuns = [&](size_t startR, size_t endR) -> bool {
        std::vector<uint8_t> buf(maxBufPerThread);
        for (size_t ri = startR; ri < endR; ++ri) {
            const auto& run = runs[ri];
            if (run.bytes > maxBufPerThread) {
                // Split oversized run into aligned superblock chunks
                uint64_t runOff = run.file_offset;
                uint64_t remaining = run.bytes;
                size_t ti = run.first_task;
                size_t maxTasksPerChunk = maxBufPerThread / sbBV;
                if (maxTasksPerChunk == 0) return false; // memory too small
                while (remaining > 0) {
                    size_t tasksThisChunk = std::min(static_cast<size_t>(run.task_count - (ti - run.first_task)), maxTasksPerChunk);
                    uint64_t chunk = tasksThisChunk * sbBV;
                    if (pread(fd, buf.data(), chunk, runOff) != static_cast<ssize_t>(chunk))
                        return false;
                    for (size_t j = 0; j < tasksThisChunk; ++j)
                        unpackLeaves(hdr, plan, plan.tasks[ti + j], buf.data() + j * sbBV, output);
                    ti += tasksThisChunk;
                    runOff += chunk;
                    remaining -= chunk;
                }
            } else {
                if (pread(fd, buf.data(), run.bytes, run.file_offset) != static_cast<ssize_t>(run.bytes))
                    return false;
                uint64_t off = 0;
                for (size_t j = 0; j < run.task_count; ++j) {
                    unpackLeaves(hdr, plan, plan.tasks[run.first_task + j], buf.data() + off, output);
                    off += sbBV;
                }
            }
        }
        return true;
    };

    if (numThreads == 1) {
        if (!processRuns(0, runs.size())) return false;
    } else {
        ThreadPool pool(static_cast<size_t>(numThreads), pinThreads);
        std::vector<std::future<bool>> futures;
        size_t nr = runs.size();
        for (int t = 0; t < numThreads; ++t) {
            futures.push_back(pool.submit([&, t]() -> bool {
                size_t start = t * nr / numThreads;
                size_t end = (t + 1) * nr / numThreads;
                return processRuns(start, end);
            }));
        }
        pool.waitAll();
        for (auto& f : futures) if (!f.get()) return false;
    }

    if (profile) {
        uint64_t totalRead = 0;
        for (const auto& r : runs) totalRead += r.bytes;
        profile->superblocks_touched = plan.superblocks_touched;
        profile->pread_calls = runs.size();
        profile->bytes_read = totalRead;
        profile->output_bytes = plan.output_bytes;
    }
    return true;
}

// ============================================================================
// LeafIndex: Read only needed leaf blocks, merge into extents
//
// 只读取实际需要的leaf blocks，按文件偏移排序后合并为连续extent
// 适用于: 需要的数据分散在多个superblock中的场景
// ============================================================================

bool executeSBPlanLeafIndex(int fd, const SBTaskPlan& plan, const ERWT3DHeader& hdr,
                             float* output, int numThreads, size_t memoryLimitMB,
                             size_t leafMergeBytes, IOProfile* profile, bool pinThreads) {
    const uint64_t sbBV = sbBytes(hdr);
    const uint64_t lfBV = lfBytes(hdr);
    size_t memLimit = memoryLimitMB * 1024ULL * 1024ULL;

    // HDD优化: 提示内核顺序访问模式
    adviseSequential(fd);
    if (leafMergeBytes < lfBV * 2) leafMergeBytes = lfBV * 16;
    if (leafMergeBytes > memLimit / 2) leafMergeBytes = memLimit / 2;
    if (leafMergeBytes < lfBV * 4) return false; // memory too small

    // Build leaf offset list from the plan
    struct LeafOff { uint64_t off; const SBTask* task; uint16_t leafIdx; };
    std::vector<LeafOff> leafOffs;
    for (const auto& task : plan.tasks) {
        for (uint16_t li = 0; li < task.leaf_count; ++li) {
            uint64_t opIdx = task.first_leaf + li;
            leafOffs.push_back({task.file_offset + plan.leaf_ops[opIdx].morton * lfBV, &task, li});
        }
    }
    if (leafOffs.empty()) return true;

    // Sort by file offset
    std::sort(leafOffs.begin(), leafOffs.end(), [](const LeafOff& a, const LeafOff& b) { return a.off < b.off; });

    // Build merged extents
    struct Ext { uint64_t off, size; size_t firstLeaf, leafCount; };
    std::vector<Ext> extents;
    for (size_t i = 0; i < leafOffs.size(); ) {
        Ext e; e.off = leafOffs[i].off; e.size = lfBV; e.firstLeaf = i; e.leafCount = 1;
        while (i + e.leafCount < leafOffs.size() &&
               leafOffs[i + e.leafCount].off <= e.off + e.size &&
               e.size + lfBV <= leafMergeBytes) {
            e.size += lfBV; ++e.leafCount;
        }
        extents.push_back(e); i += e.leafCount;
    }

    // Execute
    uint64_t totalRead = 0, totalCalls = extents.size();
    auto processExtents = [&](size_t start, size_t end) -> bool {
        std::vector<uint8_t> buf(leafMergeBytes * 2);
        for (size_t ei = start; ei < end; ++ei) {
            const auto& ext = extents[ei];
            if (ext.size > buf.size()) buf.resize(ext.size);
            if (pread(fd, buf.data(), ext.size, ext.off) != static_cast<ssize_t>(ext.size))
                return false;
            for (size_t li = 0; li < ext.leafCount; ++li) {
                const auto& lo = leafOffs[ext.firstLeaf + li];
                // Create single-leaf task to avoid unpacking all leaves
                SBTask oneLeaf = *lo.task;
                oneLeaf.first_leaf = static_cast<uint32_t>(lo.task->first_leaf + lo.leafIdx);
                oneLeaf.leaf_count = 1;
                uint64_t leafOffInBuf = lo.off - ext.off;
                // The leaf data at this offset: the buffer starts at the leaf block
                // unpackLeaves expects the buffer to contain the full superblock
                // We have only the leaf block. Use manual extraction instead.
                const LeafOp& op = plan.leaf_ops[oneLeaf.first_leaf];
                const float* leaf = reinterpret_cast<const float*>(buf.data() + leafOffInBuf);
                const uint64_t lx = hdr.leaf_x, ly = hdr.leaf_y;
                if (plan.axis == 2) { // Z
                    uint64_t srcBase = op.param * ly;
                    for (uint32_t v = 0; v < op.v_outer; ++v)
                        for (uint32_t u = 0; u < op.v_inner; ++u)
                            output[op.out_base + v * op.out_stride + u] = leaf[(srcBase + v) * lx + u];
                } else if (plan.axis == 1) { // Y
                    for (uint32_t v = 0; v < op.v_outer; ++v)
                        for (uint32_t u = 0; u < op.v_inner; ++u)
                            output[op.out_base + v * op.out_stride + u] = leaf[(v * ly + op.param) * lx + u];
                } else { // X
                    for (uint32_t v = 0; v < op.v_outer; ++v)
                        for (uint32_t u = 0; u < op.v_inner; ++u)
                            output[op.out_base + v * op.out_stride + u] = leaf[(v * ly) * lx + op.param + u * lx];
                }
            }
        }
        return true;
    };

    if (numThreads <= 1) {
        if (!processExtents(0, extents.size())) return false;
    } else {
        ThreadPool pool(static_cast<size_t>(numThreads), pinThreads);
        std::vector<std::future<bool>> futures;
        size_t ne = extents.size();
        for (int t = 0; t < numThreads; ++t) {
            futures.push_back(pool.submit([&, t]() -> bool {
                size_t s = t * ne / numThreads, e = (t + 1) * ne / numThreads;
                return processExtents(s, e);
            }));
        }
        pool.waitAll();
        for (auto& f : futures) if (!f.get()) return false;
    }

    if (profile) {
        profile->superblocks_touched = plan.superblocks_touched;
        profile->pread_calls = totalCalls;
        profile->bytes_read = 0; for (auto& e : extents) profile->bytes_read += e.size;
        profile->output_bytes = plan.output_bytes;
    }
    return true;
}

// ============================================================================
// HDDReadWindow: Large contiguous read windows with gap tolerance
//
// 将superblocks按文件偏移分组到读窗口中
// 窗口内允许gap（但不超过max_gap_bytes），减少寻道次数
// 适用于: HDD上顺序读优化
// ============================================================================

bool executeSBPlanHDDReadWindow(int fd, const SBTaskPlan& plan, const ERWT3DHeader& hdr,
                                 float* output, int numThreads, size_t memoryLimitMB,
                                 const HDDReadWindowConfig& cfg, IOProfile* profile,
                                 bool pinThreads) {
    const uint64_t sbBV = sbBytes(hdr);
    const size_t n = plan.tasks.size();
    if (n == 0) return true;
    if (numThreads <= 1) numThreads = 1;

    const uint64_t rwBytes = cfg.read_window_bytes > 0 ? cfg.read_window_bytes : 128 * 1024 * 1024;
    const uint64_t gapBytes = cfg.max_gap_bytes > 0 ? cfg.max_gap_bytes : 1024 * 1024;

    adviseSequential(fd);

    std::vector<uint64_t> offsets(n);
    for (size_t i = 0; i < n; ++i) offsets[i] = plan.tasks[i].file_offset;
    auto windows = buildWindows(offsets.data(), n, sbBV, rwBytes, gapBytes);

    size_t maxWinBytes = 0;
    for (const auto& w : windows) maxWinBytes = std::max<size_t>(maxWinBytes, static_cast<size_t>(w.read_bytes));
    size_t maxBufPerThread = memoryLimitMB * 1024ULL * 1024ULL / static_cast<size_t>(numThreads);
    if (maxBufPerThread < sbBV) return false;
    maxBufPerThread = std::min(maxBufPerThread, std::max<size_t>(maxWinBytes, static_cast<size_t>(sbBV * 4)));

    auto processWindows = [&](size_t startW, size_t endW) -> bool {
        std::vector<uint8_t> buf(maxBufPerThread);
        for (size_t wi = startW; wi < endW; ++wi) {
            const auto& win = windows[wi];

            prefetchWindows(fd, windows, wi, endW, 20);

            if (win.read_bytes > maxBufPerThread) {
                uint64_t winOff = win.file_offset;
                uint64_t remaining = win.read_bytes;
                size_t ti = win.first_task;
                size_t maxTasksPerChunk = maxBufPerThread / sbBV;
                if (maxTasksPerChunk == 0) return false;
                while (remaining > 0) {
                    size_t tasksThisChunk = std::min(
                        static_cast<size_t>(win.task_count - (ti - win.first_task)),
                        maxTasksPerChunk);
                    uint64_t chunk = tasksThisChunk * sbBV;
                    uint64_t chunkOff = winOff + (ti - win.first_task) * sbBV;
                    if (pread(fd, buf.data(), chunk, chunkOff) != static_cast<ssize_t>(chunk))
                        return false;
                    for (size_t j = 0; j < tasksThisChunk; ++j)
                        unpackLeaves(hdr, plan, plan.tasks[ti + j],
                                     buf.data() + j * sbBV, output);
                    ti += tasksThisChunk;
                    remaining -= chunk;
                }
            } else {
                if (pread(fd, buf.data(), win.read_bytes, win.file_offset) !=
                    static_cast<ssize_t>(win.read_bytes))
                    return false;
                size_t ti = win.first_task;
                for (size_t j = 0; j < win.task_count; ++j) {
                    uint64_t taskOffInBuf = plan.tasks[ti + j].file_offset - win.file_offset;
                    unpackLeaves(hdr, plan, plan.tasks[ti + j],
                                 buf.data() + taskOffInBuf, output);
                }
            }
        }
        return true;
    };

    double rd = 0;
    if (numThreads == 1) {
        auto tr0 = std::chrono::high_resolution_clock::now();
        if (!processWindows(0, windows.size())) return false;
        auto tr1 = std::chrono::high_resolution_clock::now();
        rd = std::chrono::duration<double, std::milli>(tr1 - tr0).count();
    } else {
        auto tr0 = std::chrono::high_resolution_clock::now();
        ThreadPool pool(static_cast<size_t>(numThreads), pinThreads);
        std::vector<std::future<bool>> futures;
        size_t nw = windows.size();
        for (int t = 0; t < numThreads; ++t) {
            futures.push_back(pool.submit([&, t]() -> bool {
                size_t start = t * nw / numThreads;
                size_t end = (t + 1) * nw / numThreads;
                return processWindows(start, end);
            }));
        }
        pool.waitAll();
        for (auto& f : futures) if (!f.get()) return false;
        auto tr1 = std::chrono::high_resolution_clock::now();
        rd = std::chrono::duration<double, std::milli>(tr1 - tr0).count();
    }

    if (profile) {
        uint64_t totalRead = 0, totalCalls = windows.size();
        for (const auto& w : windows) totalRead += w.read_bytes;
        profile->superblocks_touched = plan.superblocks_touched;
        profile->pread_calls = totalCalls;
        profile->bytes_read = totalRead;
        profile->output_bytes = plan.output_bytes;
        profile->read_time_ms = rd;
        profile->read_time_sum_ms = rd * numThreads;
    }
    return true;
}

// ============================================================================
// Batch HDD: Multi-slice batch planning and execution
//
// 跨多个切片全局排序任务，合并读窗口
// 适用于: 需要读取多个切片的场景
// ============================================================================

SBBatchPlan buildSBBatchPlan(const std::vector<const SBTaskPlan*>& plans) {
    SBBatchPlan bp; bp.plans = plans;
    for (uint32_t pid = 0; pid < plans.size(); ++pid) {
        bp.total_sb_touched += plans[pid]->tasks.size();
        for (const auto& t : plans[pid]->tasks)
            bp.batch_tasks.push_back({t.file_offset, t.first_leaf, t.leaf_count, pid, plans[pid]});
    }
    std::sort(bp.batch_tasks.begin(), bp.batch_tasks.end(),
        [](const SBBatchTask& a, const SBBatchTask& b) { return a.file_offset < b.file_offset; });
    return bp;
}

bool executeSBBatchHDD(int fd, const SBBatchPlan& batch, const ERWT3DHeader& hdr,
                        float* const* outputs, int numThreads, size_t memoryLimitMB,
                        const HDDReadWindowConfig& wcfg, bool pinThreads, SBBatchProfile* profile) {
    const uint64_t sbBV = sbBytes(hdr);
    const size_t n = batch.batch_tasks.size();
    if (n == 0) return true;
    if (numThreads <= 1) numThreads = 1;

    // Guard: multi-thread with shared output buffers causes data races.
    // Different windows may contain tasks for the same output_id.
    if (numThreads > 1 && batch.plans.size() > 1) {
        numThreads = 1;
    }

    const uint64_t rwB = wcfg.read_window_bytes > 0 ? wcfg.read_window_bytes : 128 * 1024 * 1024;
    const uint64_t gapB = wcfg.max_gap_bytes > 0 ? wcfg.max_gap_bytes : 1024 * 1024;

    adviseSequential(fd);

    std::vector<uint64_t> offsets(n);
    for (size_t i = 0; i < n; ++i) offsets[i] = batch.batch_tasks[i].file_offset;
    auto wins = buildWindows(offsets.data(), n, sbBV, rwB, gapB);
    size_t mwb = 0; for (auto& w : wins) mwb = std::max<size_t>(mwb, static_cast<size_t>(w.read_bytes));
    size_t mbpt = memoryLimitMB * 1024ULL * 1024ULL / static_cast<size_t>(numThreads);
    if (mbpt < sbBV) return false;
    mbpt = std::min(mbpt, std::max<size_t>(mwb, static_cast<size_t>(sbBV * 4)));

    if (profile) {
        profile->windows_count = wins.size();
        profile->superblocks_decoded = batch.batch_tasks.size();
        for (auto& w : wins) {
            profile->bytes_actual_read += w.read_bytes;
            profile->pread_calls += (w.read_bytes > mbpt) ? (w.read_bytes + mbpt - 1) / mbpt : 1;
        }
    }

    auto pw = [&](size_t sw, size_t ew) -> bool {
        std::vector<uint8_t> buf(mbpt);
        for (size_t wi = sw; wi < ew; ++wi) {
            const auto& win = wins[wi];

            prefetchWindows(fd, wins, wi, ew, 20);

            if (win.read_bytes > mbpt) {
                uint64_t wo = win.file_offset, rem = win.read_bytes; size_t ti = win.first_task;
                size_t mtpc = mbpt / sbBV; if (mtpc == 0) return false;
                while (rem > 0) {
                    size_t ttc = std::min(static_cast<size_t>(win.task_count - (ti - win.first_task)), mtpc);
                    uint64_t chunk = ttc * sbBV;
                    if (pread(fd, buf.data(), chunk, wo + (ti - win.first_task) * sbBV) != static_cast<ssize_t>(chunk))
                        return false;
                    for (size_t j = 0; j < ttc; ++j) {
                        const auto& bt = batch.batch_tasks[ti + j];
                        SBTask t{bt.file_offset, bt.first_leaf, bt.leaf_count};
                        unpackLeaves(hdr, *bt.plan, t, buf.data() + j * sbBV, outputs[bt.output_id]);
                    }
                    ti += ttc; rem -= chunk;
                }
            } else {
                if (pread(fd, buf.data(), win.read_bytes, win.file_offset) != static_cast<ssize_t>(win.read_bytes)) return false;
                for (size_t j = 0; j < win.task_count; ++j) {
                    const auto& bt = batch.batch_tasks[win.first_task + j];
                    uint64_t toff = bt.file_offset - win.file_offset;
                    SBTask t{bt.file_offset, bt.first_leaf, bt.leaf_count};
                    unpackLeaves(hdr, *bt.plan, t, buf.data() + toff, outputs[bt.output_id]);
                }
            }
        }
        return true;
    };
    if (numThreads == 1) return pw(0, wins.size());
    ThreadPool pool(static_cast<size_t>(numThreads), pinThreads);
    std::vector<std::future<bool>> futs;
    size_t nw = wins.size();
    for (int t = 0; t < numThreads; ++t)
        futs.push_back(pool.submit([&, t]() -> bool { return pw(t * nw / numThreads, (t + 1) * nw / numThreads); }));
    pool.waitAll();
    for (auto& f : futs) if (!f.get()) return false;
    return true;
}

HDDReadWindowConfig calibrateHDD(int raw_fd, uint64_t raw_size) {
    HDDReadWindowConfig cfg;
    if (raw_fd < 0 || raw_size < 1024 * 1024) return cfg;

    using Clock = std::chrono::steady_clock;

    // Sequential bandwidth: 3 regions (head, middle, tail), 256MB each
    const uint64_t seqChunk = 256ULL * 1024 * 1024;
    std::vector<char> seqBuf(seqChunk);
    std::vector<double> seqMBS;
    for (int region = 0; region < 3; ++region) {
        uint64_t off = (region == 0) ? 0 : (region == 1) ? raw_size / 2 : raw_size - seqChunk;
        if (off + seqChunk > raw_size) off = raw_size > seqChunk ? raw_size - seqChunk : 0;
        posix_fadvise(raw_fd, static_cast<off_t>(off), static_cast<off_t>(seqChunk), POSIX_FADV_DONTNEED);

        auto t0 = Clock::now();
        size_t done = 0;
        while (done < seqChunk) {
            ssize_t n = pread(raw_fd, seqBuf.data() + done, seqChunk - done, static_cast<off_t>(off + done));
            if (n <= 0) break;
            done += static_cast<size_t>(n);
        }
        auto t1 = Clock::now();
        double sec = std::chrono::duration<double>(t1 - t0).count();
        if (sec > 0.0 && done > 0) seqMBS.push_back((static_cast<double>(done) / (1024.0 * 1024.0)) / sec);
    }
    if (!seqMBS.empty()) {
        cfg.sequential_mb_s = seqMBS[seqMBS.size() / 2]; // median
    }

    // Random 1MB reads: 128 samples, 64-bit uniform random, 4KB-aligned
    const uint64_t sampleSize = 1024 * 1024;
    std::vector<char> randBuf(sampleSize);
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<uint64_t> dist(0, std::max<uint64_t>(raw_size, sampleSize) - sampleSize);
    std::vector<double> latencies;

    for (uint64_t i = 0; i < 128; ++i) {
        uint64_t off = dist(rng) & ~4095ULL;
        if (off + sampleSize > raw_size) off = raw_size - sampleSize;
        posix_fadvise(raw_fd, static_cast<off_t>(off), static_cast<off_t>(sampleSize), POSIX_FADV_DONTNEED);

        auto rt0 = Clock::now();
        ssize_t n = pread(raw_fd, randBuf.data(), sampleSize, static_cast<off_t>(off));
        auto rt1 = Clock::now();
        if (n == static_cast<ssize_t>(sampleSize)) {
            latencies.push_back(std::chrono::duration<double, std::milli>(rt1 - rt0).count());
        }
    }

    if (!latencies.empty()) {
        std::sort(latencies.begin(), latencies.end());
        cfg.seek_ms = latencies[latencies.size() / 2];
    }

    return cfg;
}

} // namespace erwt3d
