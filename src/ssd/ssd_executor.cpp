#include "erwt3d/ssd/ssd_executor.hpp"
#include "erwt3d/ssd/ssd_extent_planner.hpp"
#include "erwt3d/thread_pool.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <future>
#include <unistd.h>
#include <fcntl.h>

#ifdef ERWT3D_HAVE_LZ4
#include <lz4.h>
#endif

namespace erwt3d {
namespace {

using namespace detail;

inline void adviseSequential(int fd) {
#if defined(POSIX_FADV_SEQUENTIAL)
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#else
    (void)fd;
#endif
}

struct ExtentWithBuf {
    uint64_t offset;
    uint64_t size;
    uint8_t* buf;
};

static uint64_t alignedAllocSize(uint64_t s) {
    return (s + 4095) & ~static_cast<uint64_t>(4095);
}

} // anonymous namespace

bool executeSBBatchSSD(
    int fd,
    const SBBatchPlan& batch,
    const ERWT3DHeader& header,
    float* const* outputs,
    int readThreads,
    int decodeThreads,
    const SSDReadConfig& ssdCfg,
    SBBatchProfile* profile)
{
    if (fd < 0 || batch.batch_tasks.empty()) return false;
    if (readThreads < 1) readThreads = 1;
    if (decodeThreads < 1) decodeThreads = 1;

    const uint64_t sbBV = sbBytes(header);
    const size_t n = batch.batch_tasks.size();

    if (profile) profile->superblocks_decoded = n;

    if (decodeThreads > 1 && batch.plans.size() > 1) {
        decodeThreads = 1;
    }

    std::vector<SSDLeafRequest> leafReqs;
    leafReqs.reserve(n);

    for (const auto& bt : batch.batch_tasks) {
        SSDLeafRequest req;
        req.file_offset = bt.file_offset;
        req.record_size = sbBV;
        req.superblock_id = (bt.file_offset - header.data_offset) / sbBV;
        req.morton = 0;
        req.is_xplane = false;
        req.logical_group = static_cast<uint32_t>(bt.output_id);
        req.request_index = 0;
        req.output = outputs[bt.output_id];
        req.leaf_id = req.superblock_id;
        leafReqs.push_back(req);
    }

    SSDExtentPlanConfig planCfg;
    planCfg.read_window_bytes = ssdCfg.read_window_bytes;
    planCfg.max_gap_bytes = ssdCfg.max_gap_bytes;
    planCfg.queue_depth = ssdCfg.queue_depth;
    planCfg.buffer_pool_bytes = ssdCfg.buffer_pool_bytes;
    planCfg.estimated_bandwidth_mb_s = 1000.0;
    planCfg.io_submission_cost_us = 10.0;

    auto plan = buildSSDExtentPlan(std::move(leafReqs), planCfg);

    if (profile) {
        profile->windows_count = plan.pread_calls;
        profile->pread_calls = plan.pread_calls;
        profile->bytes_actual_read = plan.planned_read_bytes;
    }

    adviseSequential(fd);

    const uint64_t poolBytes = ssdCfg.buffer_pool_bytes > 0
        ? ssdCfg.buffer_pool_bytes : 512ULL * 1024 * 1024;

    size_t ei = 0;
    while (ei < plan.extents.size()) {
        uint64_t batchBytes = 0;
        size_t batchStart = ei;
        while (ei < plan.extents.size() &&
               batchBytes + alignedAllocSize(plan.extents[ei].size) <= poolBytes) {
            batchBytes += alignedAllocSize(plan.extents[ei].size);
            ++ei;
        }
        if (batchStart == ei && ei < plan.extents.size()) {
            batchBytes = alignedAllocSize(plan.extents[ei].size);
            ++ei;
        }

        const size_t batchCount = ei - batchStart;
        std::vector<ExtentWithBuf> batchBufs(batchCount);
        for (size_t bi = 0; bi < batchCount; ++bi) {
            const auto& ext = plan.extents[batchStart + bi];
            uint8_t* buf = static_cast<uint8_t*>(std::aligned_alloc(4096,
                static_cast<size_t>(alignedAllocSize(ext.size))));
            if (!buf) {
                for (size_t j = 0; j < bi; ++j) std::free(batchBufs[j].buf);
                return false;
            }
            batchBufs[bi] = {ext.offset, ext.size, buf};
        }

        auto cleanupBatch = [&]() {
            for (auto& eb : batchBufs) if (eb.buf) { std::free(eb.buf); eb.buf = nullptr; }
        };

        if (readThreads <= 1 || batchCount <= 1) {
            for (size_t bi = 0; bi < batchCount; ++bi) {
                ssize_t nr = pread(fd, batchBufs[bi].buf,
                                   static_cast<size_t>(batchBufs[bi].size),
                                   static_cast<off_t>(batchBufs[bi].offset));
                if (nr != static_cast<ssize_t>(batchBufs[bi].size)) {
                    cleanupBatch(); return false;
                }
            }
        } else {
            ThreadPool rp(static_cast<size_t>(readThreads));
            std::vector<std::future<bool>> futs;
            for (size_t bi = 0; bi < batchCount; ++bi) {
                futs.push_back(rp.submit([fd, &batchBufs, bi]() -> bool {
                    ssize_t nr = pread(fd, batchBufs[bi].buf,
                                       static_cast<size_t>(batchBufs[bi].size),
                                       static_cast<off_t>(batchBufs[bi].offset));
                    return nr == static_cast<ssize_t>(batchBufs[bi].size);
                }));
            }
            rp.waitAll();
            for (auto& f : futs) {
                if (!f.get()) { cleanupBatch(); return false; }
            }
        }

        {
            ThreadPool dp(static_cast<size_t>(decodeThreads));
            std::vector<std::future<bool>> dfuts;
            for (size_t bi = 0; bi < batchCount; ++bi) {
                size_t extIdx = batchStart + bi;
                dfuts.push_back(dp.submit(
                    [fd, &header, &batch, outputs, &plan, &batchBufs, sbBV, bi, extIdx]() -> bool {
                const auto& ext = plan.extents[extIdx];
                const uint8_t* readBuf = batchBufs[bi].buf;
                for (size_t li = ext.first_leaf;
                     li < ext.first_leaf + ext.leaf_count; ++li) {
                    if (li >= batch.batch_tasks.size()) continue;
                    const auto& task = batch.batch_tasks[li];
                    uint64_t sbOff = task.file_offset - ext.offset;
                    if (sbOff + sbBV > ext.size) continue;
                    SBTask t{task.file_offset, task.first_leaf, task.leaf_count};
                    unpackLeaves(header, *task.plan, t, readBuf + sbOff,
                                 outputs[task.output_id]);
                }
                (void)fd;
                return true;
                }));
            }
            dp.waitAll();
            for (auto& f : dfuts) {
                if (!f.get()) { cleanupBatch(); return false; }
            }
        }

        cleanupBatch();
    }

    return true;
}

bool executeCompressedBatchSSD(
    int fd,
    const ERWT3DHeader& header,
    const std::vector<CompressedSBInfo>& sortedCompressedSBs,
    const SBBatchPlan& batch,
    float* const* outputs,
    int readThreads,
    int decodeThreads,
    const SSDReadConfig& ssdCfg,
    SBBatchProfile* profile)
{
    if (fd < 0 || sortedCompressedSBs.empty()) return false;
    if (readThreads < 1) readThreads = 1;
    if (decodeThreads < 1) decodeThreads = 1;

    const uint64_t sbBV = sbBytes(header);
    const size_t n = sortedCompressedSBs.size();

    if (profile) profile->superblocks_decoded = n;

    std::vector<SSDLeafRequest> leafReqs;
    leafReqs.reserve(n);
    for (const auto& sb : sortedCompressedSBs) {
        SSDLeafRequest req;
        req.file_offset = sb.compressed_offset;
        req.record_size = sb.compressed_size;
        req.superblock_id = sb.sb_idx;
        req.morton = 0;
        req.is_xplane = false;
        req.leaf_id = sb.sb_idx;
        leafReqs.push_back(req);
    }

    SSDExtentPlanConfig planCfg;
    planCfg.read_window_bytes = ssdCfg.read_window_bytes;
    planCfg.max_gap_bytes = ssdCfg.max_gap_bytes;
    planCfg.queue_depth = ssdCfg.queue_depth;
    planCfg.buffer_pool_bytes = ssdCfg.buffer_pool_bytes;
    planCfg.estimated_bandwidth_mb_s = 1000.0;
    planCfg.io_submission_cost_us = 10.0;

    auto plan = buildSSDExtentPlan(std::move(leafReqs), planCfg);

    if (profile) {
        profile->windows_count = plan.pread_calls;
        profile->pread_calls = plan.pread_calls;
        profile->bytes_actual_read = plan.planned_read_bytes;
    }

    adviseSequential(fd);

    const uint64_t poolBytes = ssdCfg.buffer_pool_bytes > 0
        ? ssdCfg.buffer_pool_bytes : 512ULL * 1024 * 1024;

    struct CExtBuf { uint64_t offset, size; uint8_t* buf; size_t first, count; };

    size_t sbCursor = 0;
    size_t ei = 0;

    auto buildBatchExtBufs = [&](size_t bStart, size_t bEnd,
                                  std::vector<CExtBuf>& out) -> bool {
        out.clear();
        size_t localCursor = (bStart == 0) ? 0 : sbCursor;
        for (size_t idx = bStart; idx < bEnd; ++idx) {
            const auto& ext = plan.extents[idx];
            uint8_t* buf = static_cast<uint8_t*>(std::aligned_alloc(4096,
                static_cast<size_t>(alignedAllocSize(ext.size))));
            if (!buf) {
                for (auto& eb : out) std::free(eb.buf);
                return false;
            }
            size_t first = localCursor;
            while (localCursor < n &&
                   sortedCompressedSBs[localCursor].compressed_offset < ext.offset + ext.size)
                ++localCursor;
            out.push_back({ext.offset, ext.size, buf, first, localCursor - first});
        }
        if (bEnd > 0) sbCursor = (out.back().first + out.back().count > sbCursor)
            ? out.back().first + out.back().count : sbCursor;
        return true;
    };

    auto cleanupBatch = [](std::vector<CExtBuf>& bufs) {
        for (auto& eb : bufs) if (eb.buf) { std::free(eb.buf); eb.buf = nullptr; }
    };

    while (sbCursor < n || ei < plan.extents.size()) {
        uint64_t batchBytes = 0;
        size_t batchStart = ei;
        while (ei < plan.extents.size() &&
               batchBytes + alignedAllocSize(plan.extents[ei].size) <= poolBytes) {
            batchBytes += alignedAllocSize(plan.extents[ei].size);
            ++ei;
        }
        if (batchStart == ei && ei < plan.extents.size()) {
            batchBytes = alignedAllocSize(plan.extents[ei].size);
            ++ei;
        }

        std::vector<CExtBuf> batchBufs;
        if (!buildBatchExtBufs(batchStart, ei, batchBufs)) return false;

        if (readThreads <= 1 || batchBufs.size() <= 1) {
            for (size_t bi = 0; bi < batchBufs.size(); ++bi) {
                ssize_t nr = pread(fd, batchBufs[bi].buf,
                                   static_cast<size_t>(batchBufs[bi].size),
                                   static_cast<off_t>(batchBufs[bi].offset));
                if (nr != static_cast<ssize_t>(batchBufs[bi].size)) {
                    cleanupBatch(batchBufs); return false;
                }
            }
        } else {
            ThreadPool rp(static_cast<size_t>(readThreads));
            std::vector<std::future<bool>> futs;
            for (size_t bi = 0; bi < batchBufs.size(); ++bi) {
                futs.push_back(rp.submit([fd, &batchBufs, bi]() -> bool {
                    ssize_t nr = pread(fd, batchBufs[bi].buf,
                                       static_cast<size_t>(batchBufs[bi].size),
                                       static_cast<off_t>(batchBufs[bi].offset));
                    return nr == static_cast<ssize_t>(batchBufs[bi].size);
                }));
            }
            rp.waitAll();
            for (auto& f : futs) if (!f.get()) { cleanupBatch(batchBufs); return false; }
        }

        {
            ThreadPool dp(static_cast<size_t>(decodeThreads));
            std::vector<std::future<bool>> dfuts;
            for (size_t bi = 0; bi < batchBufs.size(); ++bi) {
                dfuts.push_back(dp.submit(
                    [fd, &header, &sortedCompressedSBs, &batch, outputs,
                     &batchBufs, sbBV, n, bi]() -> bool {
                const auto& eb = batchBufs[bi];
                std::vector<uint8_t> decompBuf(sbBV);
                for (size_t si = eb.first; si < eb.first + eb.count; ++si) {
                    if (si >= n) continue;
                    const auto& sb = sortedCompressedSBs[si];
                    if (sb.compressed_offset < eb.offset) continue;
                    uint64_t relOff = sb.compressed_offset - eb.offset;
                    if (relOff + sb.compressed_size > eb.size) continue;
                    const uint8_t* src = eb.buf + relOff;

                    if (sb.is_compressed) {
#ifdef ERWT3D_HAVE_LZ4
                        int dec = LZ4_decompress_safe(
                            reinterpret_cast<const char*>(src),
                            reinterpret_cast<char*>(decompBuf.data()),
                            static_cast<int>(sb.compressed_size),
                            static_cast<int>(sbBV));
                        if (dec != static_cast<int>(sbBV)) return false;
#else
                        return false;
#endif
                    } else {
                        std::memcpy(decompBuf.data(), src, sbBV);
                    }

                    for (size_t si2 : sb.scatter_indices) {
                        if (si2 >= batch.batch_tasks.size()) continue;
                        const auto& bt = batch.batch_tasks[si2];
                        SBTask t{bt.file_offset, bt.first_leaf, bt.leaf_count};
                        unpackLeaves(header, *bt.plan, t, decompBuf.data(),
                                     outputs[bt.output_id]);
                    }
                }
                (void)fd;
                return true;
                }));
            }
            dp.waitAll();
            for (auto& f : dfuts) if (!f.get()) { cleanupBatch(batchBufs); return false; }
        }

        cleanupBatch(batchBufs);
    }

    return true;
}

} // namespace erwt3d
