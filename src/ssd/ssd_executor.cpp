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

} // anonymous namespace

struct ExtentWithBuf {
    uint64_t offset;
    uint64_t size;
    uint8_t* buf;
};

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

    std::vector<ExtentWithBuf> extentBufs;
    extentBufs.reserve(plan.extents.size());
    for (size_t ei = 0; ei < plan.extents.size(); ++ei) {
        const auto& ext = plan.extents[ei];
        uint8_t* buf = static_cast<uint8_t*>(std::aligned_alloc(4096,
            (static_cast<size_t>(ext.size) + 4095) & ~static_cast<size_t>(4095ULL)));
        extentBufs.push_back({ext.offset, ext.size, buf});
    }

    auto doPread = [&](const ExtentWithBuf& eb) -> bool {
        ssize_t nr = pread(fd, eb.buf, static_cast<size_t>(eb.size),
                           static_cast<off_t>(eb.offset));
        return nr == static_cast<ssize_t>(eb.size);
    };

    auto cleanupBufs = [&]() {
        for (auto& eb : extentBufs) {
            if (eb.buf) std::free(eb.buf);
        }
    };

    if (readThreads == 1 && extentBufs.size() <= 1) {
        for (const auto& eb : extentBufs) {
            if (!doPread(eb)) { cleanupBufs(); return false; }
        }
    } else {
        ThreadPool readPool(static_cast<size_t>(readThreads));
        std::vector<std::future<bool>> readFutures;
        for (const auto& eb : extentBufs) {
            readFutures.push_back(readPool.submit([&]() -> bool { return doPread(eb); }));
        }
        readPool.waitAll();
        for (auto& f : readFutures) {
            if (!f.get()) { cleanupBufs(); return false; }
        }
    }

    {
        ThreadPool decodePool(static_cast<size_t>(decodeThreads));
        std::vector<std::future<void>> decodeFutures;
        for (size_t ei = 0; ei < plan.extents.size(); ++ei) {
            const auto& ext = plan.extents[ei];
            const uint8_t* readBuf = extentBufs[ei].buf;
            decodeFutures.push_back(decodePool.submit([&, ei, readBuf]() {
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
            }));
        }
        decodePool.waitAll();
    }

    cleanupBufs();
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

    struct CExtentBuf {
        uint64_t offset;
        uint64_t size;
        uint8_t* buf;
        size_t first_sb;
        size_t sb_count;
    };

    std::vector<CExtentBuf> extBufs;
    extBufs.reserve(plan.extents.size());

    size_t sbCursor = 0;
    for (size_t ei = 0; ei < plan.extents.size(); ++ei) {
        const auto& ext = plan.extents[ei];
        uint8_t* buf = static_cast<uint8_t*>(std::aligned_alloc(4096,
            (static_cast<size_t>(ext.size) + 4095) & ~static_cast<size_t>(4095ULL)));
        size_t firstSb = sbCursor;
        while (sbCursor < n &&
               sortedCompressedSBs[sbCursor].compressed_offset < ext.offset + ext.size) {
            ++sbCursor;
        }
        extBufs.push_back({ext.offset, ext.size, buf, firstSb, sbCursor - firstSb});
    }

    auto doPread = [fd](const CExtentBuf& eb) -> bool {
        ssize_t nr = pread(fd, eb.buf, static_cast<size_t>(eb.size),
                           static_cast<off_t>(eb.offset));
        return nr == static_cast<ssize_t>(eb.size);
    };

    auto cleanup = [&]() {
        for (auto& eb : extBufs) if (eb.buf) std::free(eb.buf);
    };

    if (readThreads <= 1 && extBufs.size() <= 1) {
        for (const auto& eb : extBufs) {
            if (!doPread(eb)) { cleanup(); return false; }
        }
    } else {
        ThreadPool rp(static_cast<size_t>(readThreads));
        std::vector<std::future<bool>> futs;
        for (const auto& eb : extBufs) {
            futs.push_back(rp.submit([&]() -> bool { return doPread(eb); }));
        }
        rp.waitAll();
        for (auto& f : futs) if (!f.get()) { cleanup(); return false; }
    }

    {
        ThreadPool dp(static_cast<size_t>(decodeThreads));
        std::vector<std::future<void>> dfuts;

        std::vector<std::vector<uint8_t>> workerBufs(
            static_cast<size_t>(decodeThreads),
            std::vector<uint8_t>(sbBV));

        for (size_t ei = 0; ei < extBufs.size(); ++ei) {
            const auto& eb = extBufs[ei];
            dfuts.push_back(dp.submit([&, ei]() {
                uint8_t* decompBuf = workerBufs[
                    static_cast<size_t>(ei % static_cast<size_t>(decodeThreads))].data();
                for (size_t si = eb.first_sb; si < eb.first_sb + eb.sb_count; ++si) {
                    if (si >= n) continue;
                    const auto& sb = sortedCompressedSBs[si];
                    const uint8_t* src = eb.buf +
                        (sb.compressed_offset - eb.offset);

                    if (sb.is_compressed) {
#ifdef ERWT3D_HAVE_LZ4
                        int dec = LZ4_decompress_safe(
                            reinterpret_cast<const char*>(src),
                            reinterpret_cast<char*>(decompBuf),
                            static_cast<int>(sb.compressed_size),
                            static_cast<int>(sbBV));
                        if (dec != static_cast<int>(sbBV)) return;
#else
                        return;
#endif
                    } else {
                        std::memcpy(decompBuf, src, sbBV);
                    }

                    for (size_t si2 : sb.scatter_indices) {
                        if (si2 >= batch.batch_tasks.size()) continue;
                        const auto& bt = batch.batch_tasks[si2];
                        SBTask t{bt.file_offset, bt.first_leaf, bt.leaf_count};
                        unpackLeaves(header, *bt.plan, t, decompBuf,
                                     outputs[bt.output_id]);
                    }
                }
            }));
        }
        dp.waitAll();
    }

    cleanup();
    return true;
}

} // namespace erwt3d
