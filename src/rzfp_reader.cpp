#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/morton.hpp"
#include "erwt3d/rzfp_codec.hpp"
#include "erwt3d/rzfp_strategy.hpp"
#include "erwt3d/rzfp_xplane_codec.hpp"
#include "erwt3d/sb_plan.hpp"
#include "erwt3d/thread_pool.hpp"
#include "erwt3d/raw_x_aux.hpp"
#include "erwt3d/ssd/ssd_extent_planner.hpp"
#include "erwt3d/ssd/ssd_config.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace erwt3d {

namespace {

using Clock = std::chrono::steady_clock;
constexpr uint64_t MiB = 1024ULL * 1024ULL;

static double msSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

static uint64_t nsSince(Clock::time_point t) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t).count());
}

static ERWT3DHeader planHeaderFromRzfp(const RzfpFileHeader& rh) {
    ERWT3DHeader h{};
    std::memcpy(h.magic, rh.magic, 8);
    h.version = rh.version;
    h.nx = rh.nx;
    h.ny = rh.ny;
    h.nz = rh.nz;
    h.dtype = rh.dtype;
    h.super_x = rh.super_x;
    h.super_y = rh.super_y;
    h.super_z = rh.super_z;
    h.leaf_x = rh.leaf_x;
    h.leaf_y = rh.leaf_y;
    h.leaf_z = rh.leaf_z;
    h.data_offset = rh.data_offset;
    h.flags = rh.flags;
    return h;
}

static uint64_t physicalSuperblockId(const RzfpFileHeader& rh, uint64_t logical_id) {
    const uint64_t sgX = rzfpSuperGridX(rh);
    const uint64_t sgY = rzfpSuperGridY(rh);
    const uint64_t sx = logical_id % sgX;
    const uint64_t rem = logical_id / sgX;
    const uint64_t sy = rem % sgY;
    const uint64_t sz = rem / sgY;
    return rzfpSuperblockId(
        rh,
        sz,
        sy,
        sx,
        (rh.flags & FLAG_PHYSICAL_ORDER_YZX)
            ? PhysicalOrder::V05_YZX
            : PhysicalOrder::ZYX
    );
}

static uint64_t combineFileIdentity(const struct stat& st, uint64_t fileSize) {
    uint64_t h = static_cast<uint64_t>(st.st_dev);
    h ^= static_cast<uint64_t>(st.st_ino) +
         0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h ^= fileSize + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

struct ScatterRef {
    LeafOp op{};
    float* output = nullptr;
};

struct RzfpLeafTask {
    uint64_t physical_sb_id = 0;
    uint16_t morton = 0;
    uint64_t file_offset = 0;
    uint16_t record_size = 0;
    RzfpLeafCodec codec = RzfpLeafCodec::RawFloat32;
    std::vector<ScatterRef> scatters;
};

struct ReadInterval {
    uint64_t offset = 0;
    uint64_t size = 0;
    uint64_t user = 0;
};

using DecodeCallback =
    std::function<bool(uint64_t user, const uint8_t* data, RzfpCodec& codec)>;

static std::vector<RzfpLeafTask> buildLeafTasks(
    const ERWT3DHeader& plan_hdr,
    const std::vector<RzfpReader::SliceBatchRequest>& requests,
    const RzfpFileHeader& header,
    double& plan_time_ms,
    RzfpReadProfile* profile = nullptr
) {
    const auto t0 = Clock::now();

    std::vector<SBTaskPlan> plans;
    std::vector<float*> outputs;
    plans.reserve(requests.size());
    outputs.reserve(requests.size());

    for (const auto& request : requests) {
        switch (request.axis) {
            case SliceAxis::X:
                plans.push_back(buildSBPlanX(plan_hdr, request.index));
                break;
            case SliceAxis::Y:
                plans.push_back(buildSBPlanY(plan_hdr, request.index));
                break;
            case SliceAxis::Z:
                plans.push_back(buildSBPlanZ(plan_hdr, request.index));
                break;
        }
        outputs.push_back(request.output);
    }

    std::vector<RzfpLeafTask> tasks;
    tasks.reserve(1024);
    std::unordered_map<uint64_t, size_t> taskMap;
    taskMap.reserve(1024);

    for (size_t p = 0; p < plans.size(); ++p) {
        const auto& plan = plans[p];
        float* output = outputs[p];
        for (const auto& task : plan.tasks) {
            const uint64_t physicalSb =
                physicalSuperblockId(header, task.sb_index);
            const LeafOp* ops = plan.leaf_ops.data() + task.first_leaf;
            for (uint32_t li = 0; li < task.leaf_count; ++li) {
                const LeafOp& op = ops[li];
                if (profile) {
                    ++profile->logical_leaf_requests;
                    profile->unique_leaf_requests = taskMap.size();
                }
                const uint64_t key = (physicalSb << 16) | op.morton;
                const auto it = taskMap.find(key);
                if (it == taskMap.end()) {
                    RzfpLeafTask leafTask;
                    leafTask.physical_sb_id = physicalSb;
                    leafTask.morton = op.morton;
                    leafTask.scatters.push_back({op, output});
                    const size_t index = tasks.size();
                    tasks.push_back(std::move(leafTask));
                    taskMap.emplace(key, index);
                } else {
                    tasks[it->second].scatters.push_back({op, output});
                }
            }
        }
    }
    if (profile) {
        profile->unique_leaf_requests = tasks.size();
        profile->duplicate_leaf_requests =
            profile->logical_leaf_requests > profile->unique_leaf_requests
                ? profile->logical_leaf_requests - profile->unique_leaf_requests
                : 0;
    }

    plan_time_ms = msSince(t0);
    return tasks;
}

static constexpr uint32_t PREFIX_CHECKPOINT_STRIDE = 16;
using PrefixCheckpointMap =
    std::unordered_map<uint64_t, std::vector<uint32_t>>;

static PrefixCheckpointMap buildPrefixCheckpoints(
    const std::vector<RzfpLeafTask>& tasks,
    const std::vector<RzfpLeafDescriptor>& descriptors,
    uint64_t leavesPerSB
) {
    PrefixCheckpointMap checkpoints;
    checkpoints.reserve(tasks.size());
    std::unordered_set<uint64_t> seen;

    for (const auto& task : tasks) {
        if (!seen.insert(task.physical_sb_id).second) continue;

        const uint64_t count =
            (leavesPerSB + PREFIX_CHECKPOINT_STRIDE - 1) /
                PREFIX_CHECKPOINT_STRIDE +
            1;
        std::vector<uint32_t> checkpoint(count, 0);
        const uint64_t descriptorBase = task.physical_sb_id * leavesPerSB;
        uint32_t running = 0;

        for (uint64_t i = 0; i < leavesPerSB; ++i) {
            if (i % PREFIX_CHECKPOINT_STRIDE == 0) {
                checkpoint[i / PREFIX_CHECKPOINT_STRIDE] = running;
            }
            running += descriptorSize(descriptors[descriptorBase + i]);
        }
        checkpoint[count - 1] = running;
        checkpoints.emplace(task.physical_sb_id, std::move(checkpoint));
    }

    return checkpoints;
}

static uint32_t prefixFromCheckpoint(
    uint16_t morton,
    const std::vector<uint32_t>& checkpoints,
    const std::vector<RzfpLeafDescriptor>& descriptors,
    uint64_t descriptorBase
) {
    const uint32_t checkpointIndex = morton / PREFIX_CHECKPOINT_STRIDE;
    uint32_t base = checkpoints[checkpointIndex];
    const uint64_t start =
        static_cast<uint64_t>(checkpointIndex) * PREFIX_CHECKPOINT_STRIDE;
    for (uint64_t i = start; i < morton; ++i) {
        base += descriptorSize(descriptors[descriptorBase + i]);
    }
    return base;
}

static void computeTaskOffsets(
    std::vector<RzfpLeafTask>& tasks,
    const std::vector<RzfpLeafDescriptor>& descriptors,
    const std::vector<RzfpSuperblockIndex>& sbIndex,
    uint64_t leavesPerSB,
    const PrefixCheckpointMap& checkpoints,
    double& prefix_time_ms
) {
    const auto t0 = Clock::now();

    for (auto& task : tasks) {
        const uint64_t descriptorBase = task.physical_sb_id * leavesPerSB;
        const auto descriptor = descriptors[descriptorBase + task.morton];
        task.codec = descriptorCodec(descriptor);
        task.record_size = descriptorSize(descriptor);
        const uint32_t prefix = prefixFromCheckpoint(
            task.morton,
            checkpoints.at(task.physical_sb_id),
            descriptors,
            descriptorBase
        );
        task.file_offset =
            sbIndex[task.physical_sb_id].payload_offset + prefix;
    }

    prefix_time_ms = msSince(t0);
}

static std::unordered_map<uint64_t, std::vector<size_t>> groupTasksBySuperblock(
    const std::vector<RzfpLeafTask>& tasks
) {
    std::unordered_map<uint64_t, std::vector<size_t>> groups;
    groups.reserve(tasks.size());
    for (size_t i = 0; i < tasks.size(); ++i) {
        groups[tasks[i].physical_sb_id].push_back(i);
    }
    return groups;
}

static uint64_t estimateSelectiveBytes(
    const std::vector<RzfpLeafTask>& tasks,
    uint64_t readWindow,
    uint64_t maxGap,
    uint64_t& preadCalls
) {
    preadCalls = 0;
    uint64_t bytes = 0;
    size_t i = 0;

    while (i < tasks.size()) {
        const uint64_t windowStart = tasks[i].file_offset;
        uint64_t windowEnd = windowStart + tasks[i].record_size;
        size_t j = i + 1;

        while (j < tasks.size()) {
            const uint64_t offset = tasks[j].file_offset;
            const uint64_t end = offset + tasks[j].record_size;
            if (offset > windowEnd + maxGap) break;
            if (end - windowStart > readWindow) break;
            windowEnd = std::max(windowEnd, end);
            ++j;
        }

        bytes += windowEnd - windowStart;
        ++preadCalls;
        i = j;
    }

    return bytes;
}

static uint64_t totalPayloadBytes(
    const std::vector<RzfpSuperblockIndex>& sbIndex
) {
    uint64_t total = 0;
    for (const auto& sb : sbIndex) {
        total += sb.payload_bytes;
    }
    return total;
}

static uint64_t estimateWholeWindowBytes(
    const std::vector<RzfpLeafTask>& tasks,
    const std::vector<RzfpSuperblockIndex>& sbIndex,
    uint64_t readWindow,
    uint64_t maxGap,
    uint64_t& preadCalls
) {
    std::unordered_set<uint64_t> touched;
    for (const auto& task : tasks) {
        touched.insert(task.physical_sb_id);
    }

    std::vector<ReadInterval> intervals;
    intervals.reserve(touched.size());
    for (uint64_t sbid : touched) {
        intervals.push_back({
            sbIndex[sbid].payload_offset,
            sbIndex[sbid].payload_bytes,
            sbid
        });
    }
    std::sort(
        intervals.begin(),
        intervals.end(),
        [](const ReadInterval& a, const ReadInterval& b) {
            if (a.offset != b.offset) return a.offset < b.offset;
            return a.size > b.size;
        }
    );

    preadCalls = 0;
    uint64_t bytes = 0;
    for (size_t i = 0; i < intervals.size();) {
        const uint64_t windowStart = intervals[i].offset;
        uint64_t windowEnd = windowStart + intervals[i].size;
        size_t j = i + 1;
        while (j < intervals.size()) {
            const uint64_t offset = intervals[j].offset;
            const uint64_t end = offset + intervals[j].size;
            if (offset > windowEnd + maxGap) break;
            if (end - windowStart > readWindow) break;
            windowEnd = std::max(windowEnd, end);
            ++j;
        }
        bytes += windowEnd - windowStart;
        ++preadCalls;
        i = j;
    }
    return bytes;
}

static StrategyDecision buildAdaptiveDecision(
    int fd,
    const std::vector<RzfpLeafTask>& tasks,
    const std::vector<RzfpSuperblockIndex>& sbIndex,
    const RzfpReaderConfig& config,
    const DeviceProfile& deviceProfile,
    RzfpReadProfile& profile
) {
    const uint64_t readWindow = config.hdd.read_window_bytes > 0
        ? config.hdd.read_window_bytes
        : 512ULL * MiB;
    const uint64_t maxGap = config.hdd.max_gap_bytes > 0
        ? config.hdd.max_gap_bytes
        : 8ULL * MiB;

    double sequential = config.hdd.sequential_mb_s;
    if (sequential <= 0.0 && config.adaptive.auto_calibrate_device) {
        sequential = deviceProfile.sequential_mb_s;
    }
    if (sequential <= 1.0) sequential = 80.0;

    double seek = config.hdd.seek_ms;
    if (seek <= 0.0 && deviceProfile.random_seek_ms > 0.0) {
        seek = deviceProfile.random_seek_ms;
    }
    if (seek <= 0.0) seek = 12.0;

    uint64_t selectivePreads = 0;
    uint64_t wholePreads = 0;

    StrategyCostInput input;
    input.selective_bytes = estimateSelectiveBytes(
        tasks,
        readWindow,
        maxGap,
        selectivePreads
    );
    input.selective_preads = selectivePreads;
    input.whole_bytes = estimateWholeWindowBytes(
        tasks,
        sbIndex,
        readWindow,
        maxGap,
        wholePreads
    );
    input.whole_preads = wholePreads;
    input.fullscan_bytes = totalPayloadBytes(sbIndex);
    input.fullscan_preads = std::max<uint64_t>(
        1,
        (input.fullscan_bytes + readWindow - 1) / readWindow
    );
    input.decoded_records = tasks.size();
    input.sequential_mb_s = sequential;
    input.seek_ms = seek;

    StrategyDecision decision =
        chooseAdaptiveStrategyFromCosts(input, config.adaptive);

    profile.predicted_selective_seconds =
        decision.selective.total_seconds;
    profile.predicted_whole_seconds =
        decision.whole.total_seconds;
    profile.predicted_fullscan_seconds =
        decision.fullscan.total_seconds;
    profile.effective_device_mb_s = sequential;

    if (decision.uncertain &&
        config.adaptive.enable_strategy_probe &&
        config.adaptive.strategy_probe_bytes > 0 &&
        config.adaptive.strategy_probe_max_seconds > 0.0 &&
        fd >= 0 &&
        !sbIndex.empty()) {
        uint64_t payloadStart = std::numeric_limits<uint64_t>::max();
        uint64_t payloadEnd = 0;
        for (const auto& sb : sbIndex) {
            payloadStart = std::min(payloadStart, sb.payload_offset);
            uint64_t end = 0;
            if (checkedAddU64(sb.payload_offset, sb.payload_bytes, end)) {
                payloadEnd = std::max(payloadEnd, end);
            }
        }

        if (payloadStart < payloadEnd) {
            const double estimatedProbeBytes =
                sequential * MiB *
                config.adaptive.strategy_probe_max_seconds;
            const uint64_t timeCappedBytes =
                estimatedProbeBytes > 0.0
                    ? static_cast<uint64_t>(estimatedProbeBytes)
                    : config.adaptive.strategy_probe_bytes;
            const uint64_t probeBytes = std::min<uint64_t>({
                config.adaptive.strategy_probe_bytes,
                payloadEnd - payloadStart,
                std::max<uint64_t>(1ULL * MiB, timeCappedBytes)
            });

            if (probeBytes > 0) {
                if (config.adaptive.cache_policy != CachePolicy::WarmAllowed) {
                    (void)posix_fadvise(
                        fd,
                        static_cast<off_t>(payloadStart),
                        static_cast<off_t>(probeBytes),
                        POSIX_FADV_DONTNEED
                    );
                }

                std::vector<uint8_t> probe(probeBytes);
                const auto t0 = Clock::now();
                const bool ok = readFullyAt(
                    fd,
                    probe.data(),
                    probeBytes,
                    payloadStart
                );
                const double elapsedSeconds =
                    std::chrono::duration<double>(Clock::now() - t0).count();

                if (ok && elapsedSeconds > 0.0) {
                    const double observed =
                        (static_cast<double>(probeBytes) / MiB) /
                        elapsedSeconds;
                    profile.pilot_observed_mb_s = observed;
                    applyObservedBandwidth(
                        decision,
                        observed,
                        config.adaptive
                    );
                    profile.predicted_selective_seconds =
                        decision.selective.total_seconds;
                    profile.predicted_whole_seconds =
                        decision.whole.total_seconds;
                    profile.predicted_fullscan_seconds =
                        decision.fullscan.total_seconds;
                    profile.effective_device_mb_s = observed;
                }

                if (config.adaptive.cache_policy != CachePolicy::WarmAllowed) {
                    (void)posix_fadvise(
                        fd,
                        static_cast<off_t>(payloadStart),
                        static_cast<off_t>(probeBytes),
                        POSIX_FADV_DONTNEED
                    );
                }
            }
        }
    }

    profile.strategy_reason = decision.reason;
    return decision;
}

static bool executeWindowedRead(
    int fd,
    const std::vector<ReadInterval>& intervals,
    const RzfpReaderConfig& config,
    DecodeCallback decodeCallback,
    RzfpReadProfile& profile
) {
    if (intervals.empty()) return true;

    auto sorted = intervals;
    std::sort(
        sorted.begin(),
        sorted.end(),
        [](const ReadInterval& a, const ReadInterval& b) {
            if (a.offset != b.offset) return a.offset < b.offset;
            return a.size > b.size;
        }
    );

    const uint64_t readWindow = config.hdd.read_window_bytes > 0
        ? config.hdd.read_window_bytes
        : 512ULL * MiB;
    const uint64_t maxGap = config.hdd.max_gap_bytes > 0
        ? config.hdd.max_gap_bytes
        : 8ULL * MiB;
    const int decodeThreads = std::max(1, config.decode_threads);

    const auto computeWindow = [&](size_t start) {
        const uint64_t windowStart = sorted[start].offset;
        uint64_t windowEnd = windowStart + sorted[start].size;
        size_t next = start + 1;
        while (next < sorted.size()) {
            const uint64_t offset = sorted[next].offset;
            const uint64_t end = offset + sorted[next].size;
            if (offset > windowEnd + maxGap) break;
            if (end - windowStart > readWindow) break;
            windowEnd = std::max(windowEnd, end);
            ++next;
        }
        return std::make_tuple(
            windowStart,
            windowEnd - windowStart,
            next
        );
    };

    std::vector<std::unique_ptr<RzfpCodec>> codecs;
    codecs.reserve(static_cast<size_t>(decodeThreads));
    for (int t = 0; t < decodeThreads; ++t) {
        codecs.emplace_back(std::make_unique<RzfpCodec>());
    }

    ThreadPool decodePool(static_cast<size_t>(decodeThreads), false);
    ThreadPool ioPool(1, false);

    const auto readWindowData = [&](
        std::vector<uint8_t>& destination,
        uint64_t windowStart,
        uint64_t windowSize
    ) -> bool {
        const WindowCacheKey key{
            config.window_cache_file_identity,
            windowStart,
            windowSize
        };

        bool cacheHit = false;
        if (config.use_window_cache && config.window_cache) {
            std::shared_ptr<const std::vector<uint8_t>> cached;
            uint64_t cachedOffset = 0;
            if (config.window_cache->getContaining(
                    config.window_cache_file_identity,
                    windowStart, windowSize,
                    cached, &cachedOffset)) {
                if (!cached) return false;
                if (cachedOffset > windowStart) return false;
                const uint64_t relative = windowStart - cachedOffset;
                if (windowSize > cached->size() - relative) return false;
                destination.resize(static_cast<size_t>(windowSize));
                std::memcpy(
                    destination.data(),
                    cached->data() + relative,
                    static_cast<size_t>(windowSize)
                );
                if (cachedOffset == windowStart && cached->size() == windowSize) {
                    ++profile.window_cache_hits;
                } else {
                    ++profile.window_cache_contained_hits;
                }
                profile.window_cache_saved_read_bytes += windowSize;
                profile.window_cache_resident_bytes =
                    config.window_cache->residentBytes();
                cacheHit = true;
            } else {
                ++profile.window_cache_misses;
            }
        }

        if (!cacheHit) {
            destination.resize(static_cast<size_t>(windowSize));
        const auto t0 = Clock::now();
        const bool ok = readFullyAt(
            fd,
            destination.data(),
            windowSize,
            windowStart
        );
        profile.io_time_ms += msSince(t0);
        if (!ok) return false;

        ++profile.pread_calls;
        profile.actual_read_bytes += windowSize;

        if (config.use_window_cache && config.window_cache &&
            windowSize <= config.window_cache->capacityBytes()) {
            auto shared = std::make_shared<const std::vector<uint8_t>>(
                destination.begin(),
                destination.end()
            );
            (void)config.window_cache->putShared(key, std::move(shared));
            profile.window_cache_resident_bytes =
                config.window_cache->residentBytes();
        }
        }

        return true;
    };

    std::vector<uint8_t> bufferA;
    std::vector<uint8_t> bufferB;

    size_t currentBegin = 0;
    auto [currentStart, currentSize, currentEnd] =
        computeWindow(currentBegin);
    if (!readWindowData(bufferA, currentStart, currentSize)) return false;

    while (true) {
        const bool hasNext = currentEnd < sorted.size();
        uint64_t nextStart = 0;
        uint64_t nextSize = 0;
        size_t nextEnd = currentEnd;
        std::future<bool> ioFuture;

        if (hasNext) {
            std::tie(nextStart, nextSize, nextEnd) =
                computeWindow(currentEnd);
            ioFuture = ioPool.submit([&, nextStart, nextSize]() {
                return readWindowData(bufferB, nextStart, nextSize);
            });
        }

        const auto decodeStart = Clock::now();
        const size_t count = currentEnd - currentBegin;
        const int threadsToUse = static_cast<int>(
            std::min<size_t>(decodeThreads, count)
        );
        bool decodeOk = true;

        if (threadsToUse <= 1) {
            for (size_t k = currentBegin; k < currentEnd; ++k) {
                const auto& interval = sorted[k];
                if (!decodeCallback(
                        interval.user,
                        bufferA.data() +
                            (interval.offset - currentStart),
                        *codecs[0])) {
                    decodeOk = false;
                }
            }
        } else {
            std::vector<std::future<bool>> futures;
            const size_t perThread =
                (count + static_cast<size_t>(threadsToUse) - 1) /
                static_cast<size_t>(threadsToUse);

            for (int t = 0; t < threadsToUse; ++t) {
                const size_t start =
                    currentBegin + static_cast<size_t>(t) * perThread;
                const size_t end = std::min(start + perThread, currentEnd);
                if (start >= end) break;

                futures.push_back(decodePool.submit(
                    [&, start, end, t, currentStart]() {
                        bool ok = true;
                        for (size_t k = start; k < end; ++k) {
                            const auto& interval = sorted[k];
                            if (!decodeCallback(
                                    interval.user,
                                    bufferA.data() +
                                        (interval.offset - currentStart),
                                    *codecs[t])) {
                                ok = false;
                            }
                        }
                        return ok;
                    }
                ));
            }

            for (auto& future : futures) {
                if (!future.get()) decodeOk = false;
            }
        }

        profile.decode_time_ms += msSince(decodeStart);
        if (!decodeOk) return false;
        if (!hasNext) break;

        if (!ioFuture.get()) {
            std::cerr << "Error: RZFP read window failed at offset "
                      << nextStart << std::endl;
            return false;
        }

        std::swap(bufferA, bufferB);
        currentBegin = currentEnd;
        currentStart = nextStart;
        currentSize = nextSize;
        currentEnd = nextEnd;
    }

    profile.scatter_time_ms =
        static_cast<double>(profile.scatter_ns.load()) * 1e-6;
    if (config.window_cache) {
        profile.window_cache_resident_bytes =
            config.window_cache->residentBytes();
    }
    return true;
}

static bool executeSelectiveLeafSSD(
    int fd,
    const std::vector<RzfpLeafTask>& tasks,
    const ERWT3DHeader& planHeader,
    const RzfpReaderConfig& config,
    RzfpReadProfile& profile)
{
    if (tasks.empty()) return true;

    std::vector<SSDLeafRequest> leafReqs;
    leafReqs.reserve(tasks.size());
    for (size_t i = 0; i < tasks.size(); ++i) {
        const auto& t = tasks[i];
        SSDLeafRequest req;
        req.file_offset = t.file_offset;
        req.record_size = t.record_size;
        req.superblock_id = t.physical_sb_id;
        req.morton = t.morton;
        req.is_xplane = false;
        req.leaf_id = (static_cast<uint64_t>(t.physical_sb_id) << 16) | t.morton;
        profile.requested_record_bytes += t.record_size;
        leafReqs.push_back(req);
    }

    SSDExtentPlanConfig planCfg;
    planCfg.read_window_bytes = config.ssd.read_window_bytes;
    planCfg.max_gap_bytes = config.ssd.max_gap_bytes;
    planCfg.queue_depth = config.ssd.queue_depth;
    planCfg.buffer_pool_bytes = config.ssd.buffer_pool_bytes;
    planCfg.estimated_bandwidth_mb_s = 2000.0;
    planCfg.io_submission_cost_us = 5.0;

    auto plan = buildSSDExtentPlan(std::move(leafReqs), planCfg);

    profile.pread_calls = plan.pread_calls;
    profile.actual_read_bytes = plan.planned_read_bytes;

#if defined(POSIX_FADV_SEQUENTIAL)
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif

    struct ExtBuf {
        uint64_t offset, size;
        uint8_t* buf;
        size_t first_task, task_count;
    };

    const uint64_t poolBytes = config.ssd.buffer_pool_bytes > 0
        ? config.ssd.buffer_pool_bytes : 512ULL * 1024 * 1024;

    const auto decodeStart = Clock::now();
    int decodeT = config.decode_threads;
    if (decodeT < 1) decodeT = 1;

    size_t extIdx = 0;
    size_t taskIdx = 0;

    while (extIdx < plan.extents.size()) {
        uint64_t batchBytes = 0;
        size_t batchStart = extIdx;
        size_t batchTaskStart = taskIdx;
        while (extIdx < plan.extents.size() &&
               batchBytes + ((plan.extents[extIdx].size + 4095) & ~static_cast<uint64_t>(4095)) <= poolBytes) {
            batchBytes += (plan.extents[extIdx].size + 4095) & ~static_cast<uint64_t>(4095);
            ++extIdx;
        }
        if (batchStart == extIdx && extIdx < plan.extents.size()) {
            batchBytes = (plan.extents[extIdx].size + 4095) & ~static_cast<uint64_t>(4095);
            ++extIdx;
        }

        const size_t batchCount = extIdx - batchStart;
        std::vector<ExtBuf> batchBufs(batchCount);
        size_t tc = taskIdx;
        for (size_t bi = 0; bi < batchCount; ++bi) {
            const auto& ext = plan.extents[batchStart + bi];
            uint8_t* buf = static_cast<uint8_t*>(std::aligned_alloc(4096,
                static_cast<size_t>((ext.size + 4095) & ~static_cast<uint64_t>(4095))));
            if (!buf) {
                for (size_t j = 0; j < bi; ++j) std::free(batchBufs[j].buf);
                return false;
            }
            size_t first = tc;
            while (tc < tasks.size() &&
                   tasks[tc].file_offset < ext.offset + ext.size) ++tc;
            batchBufs[bi] = {ext.offset, ext.size, buf, first, tc - first};
        }
        taskIdx = tc;

        auto cleanup = [&]() {
            for (auto& eb : batchBufs) if (eb.buf) { std::free(eb.buf); eb.buf = nullptr; }
        };

        int readT = config.ssd.read_threads;
        if (readT < 1) readT = 1;
        if (readT <= 1 || batchCount <= 1) {
            for (size_t bi = 0; bi < batchCount; ++bi) {
                ssize_t n = pread(fd, batchBufs[bi].buf,
                                  static_cast<size_t>(batchBufs[bi].size),
                                  static_cast<off_t>(batchBufs[bi].offset));
                if (n != static_cast<ssize_t>(batchBufs[bi].size)) {
                    cleanup(); return false;
                }
            }
        } else {
            ThreadPool rp(static_cast<size_t>(readT));
            std::vector<std::future<bool>> futs;
            for (size_t bi = 0; bi < batchCount; ++bi) {
                futs.push_back(rp.submit([fd, &batchBufs, bi]() -> bool {
                    ssize_t n = pread(fd, batchBufs[bi].buf,
                                      static_cast<size_t>(batchBufs[bi].size),
                                      static_cast<off_t>(batchBufs[bi].offset));
                    return n == static_cast<ssize_t>(batchBufs[bi].size);
                }));
            }
            rp.waitAll();
            for (auto& f : futs) if (!f.get()) { cleanup(); return false; }
        }

        {
            ThreadPool dp(static_cast<size_t>(decodeT));
            std::vector<std::future<bool>> dfuts;
            for (size_t bi = 0; bi < batchCount; ++bi) {
                dfuts.push_back(dp.submit(
                    [fd, &planHeader, &tasks, &batchBufs, bi]() -> bool {
                const auto& eb = batchBufs[bi];
                RzfpCodec codec;
                for (size_t ti = eb.first_task;
                     ti < eb.first_task + eb.task_count; ++ti) {
                    if (ti >= tasks.size()) continue;
                    const auto& task = tasks[ti];
                    if (task.file_offset < eb.offset) continue;
                    uint64_t off = task.file_offset - eb.offset;
                    if (off + task.record_size > eb.size) continue;

                    const uint8_t* data = eb.buf + off;
                    float leaf[64];
                    if (!codec.decodeRecord(
                            task.codec, data, task.record_size, leaf)) {
                        return false;
                    }

                    for (const auto& scatter : task.scatters) {
                        scatterDecodedLeaf(planHeader, scatter.op, leaf,
                                           scatter.output);
                    }
                }
                (void)fd;
                return true;
                }));
            }
            dp.waitAll();
            for (auto& f : dfuts) if (!f.get()) { cleanup(); return false; }
        }

        cleanup();
    }

    const auto decodeEnd = Clock::now();
    profile.decode_time_ms += msSince(decodeStart);
    profile.scatter_time_ms += msSince(decodeEnd);

    return true;
}

static bool executeSelectiveLeaf(
    int fd,
    const std::vector<RzfpLeafTask>& tasks,
    const ERWT3DHeader& planHeader,
    const RzfpReaderConfig& config,
    RzfpReadProfile& profile
) {
    std::vector<ReadInterval> intervals;
    intervals.reserve(tasks.size());
    for (size_t i = 0; i < tasks.size(); ++i) {
        intervals.push_back({
            tasks[i].file_offset,
            tasks[i].record_size,
            i
        });
        profile.requested_record_bytes += tasks[i].record_size;
    }

    const auto decode = [&](
        uint64_t user,
        const uint8_t* data,
        RzfpCodec& codec
    ) -> bool {
        const auto& task = tasks[user];
        float leaf[64];
        if (!codec.decodeRecord(task.codec, data, task.record_size, leaf, &profile.codec_profile)) {
                   // direct read fallback
            std::vector<uint8_t> direct(task.record_size);
            if (readFullyAt(fd, direct.data(), task.record_size, task.file_offset)) {
                if (codec.decodeRecord(task.codec, direct.data(),
                                       task.record_size, leaf, &profile.codec_profile))
                    goto scatter_direct;
                if (std::memcmp(data, direct.data(), task.record_size) == 0) {
                    std::cerr << "Error: RZFP decode failed for sb="
                              << task.physical_sb_id
                              << " morton=" << task.morton
                              << " record_size=" << task.record_size
                              << " codec=" << static_cast<int>(task.codec)
                              << " file_offset=" << task.file_offset
                              << " (identical bytes, codec issue)" << std::endl;
                } else {
                    std::cerr << "Error: RZFP decode failed for sb="
                              << task.physical_sb_id
                              << " morton=" << task.morton
                              << " record_size=" << task.record_size
                              << " codec=" << static_cast<int>(task.codec)
                              << " file_offset=" << task.file_offset
                              << " (window vs direct mismatch, first_bytes: win["
                              << static_cast<int>(data[0]) << " " << static_cast<int>(data[1])
                              << " " << static_cast<int>(data[2]) << " " << static_cast<int>(data[3])
                              << "] dir["
                              << static_cast<int>(direct[0]) << " " << static_cast<int>(direct[1])
                              << " " << static_cast<int>(direct[2]) << " " << static_cast<int>(direct[3])
                              << "])" << std::endl;
                }
                if (codec.decodeRecord(task.codec, direct.data(),
                                       task.record_size, leaf, &profile.codec_profile)) {
                    goto scatter_direct;
                }
            }
            std::cerr << "Error: RZFP decode failed for sb="
                      << task.physical_sb_id
                      << " morton=" << task.morton << std::endl;
            return false;
        }
scatter_direct:
scatter:
        const auto scatterStart = Clock::now();
        for (const auto& scatter : task.scatters) {
            scatterDecodedLeaf(planHeader, scatter.op, leaf, scatter.output);
        }
        profile.scatter_ns.fetch_add(
            nsSince(scatterStart),
            std::memory_order_relaxed
        );
        return true;
    };

    bool ok = executeWindowedRead(fd, intervals, config, decode, profile);
    return ok;
}

static bool executeWholeSuperblock(
    int fd,
    const std::vector<RzfpLeafTask>& tasks,
    const PrefixCheckpointMap& checkpoints,
    const std::vector<RzfpLeafDescriptor>& descriptors,
    const std::vector<RzfpSuperblockIndex>& sbIndex,
    const ERWT3DHeader& planHeader,
    const RzfpReaderConfig& config,
    RzfpReadProfile& profile,
    uint64_t leavesPerSB
) {
    const auto groups = groupTasksBySuperblock(tasks);

    std::vector<ReadInterval> intervals;
    intervals.reserve(groups.size());
    for (const auto& group : groups) {
        const uint64_t sbid = group.first;
        intervals.push_back({
            sbIndex[sbid].payload_offset,
            sbIndex[sbid].payload_bytes,
            sbid
        });
        for (size_t taskIndex : group.second) {
            profile.requested_record_bytes +=
                tasks[taskIndex].record_size;
        }
    }

    const auto decode = [&](
        uint64_t sbid,
        const uint8_t* payload,
        RzfpCodec& codec
    ) -> bool {
        const auto groupIt = groups.find(sbid);
        if (groupIt == groups.end()) return true;

        const auto& checkpoint = checkpoints.at(sbid);
        const uint64_t descriptorBase = sbid * leavesPerSB;
        bool ok = true;

        for (size_t taskIndex : groupIt->second) {
            const auto& task = tasks[taskIndex];
            const uint32_t offset = prefixFromCheckpoint(
                task.morton,
                checkpoint,
                descriptors,
                descriptorBase
            );
            float leaf[64];
            if (!codec.decodeRecord(
                    task.codec,
                    payload + offset,
                    task.record_size,
                    leaf)) {
                std::cerr << "Error: RZFP decode failed for sb="
                          << task.physical_sb_id
                          << " morton=" << task.morton << std::endl;
                ok = false;
                continue;
            }

            const auto scatterStart = Clock::now();
            for (const auto& scatter : task.scatters) {
                scatterDecodedLeaf(
                    planHeader,
                    scatter.op,
                    leaf,
                    scatter.output
                );
            }
            profile.scatter_ns.fetch_add(
                nsSince(scatterStart),
                std::memory_order_relaxed
            );
        }

        return ok;
    };

    return executeWindowedRead(fd, intervals, config, decode, profile);
}

static bool executeFullPayloadScan(
    int fd,
    const std::vector<RzfpLeafTask>& tasks,
    const std::vector<RzfpSuperblockIndex>& sbIndex,
    const std::vector<RzfpLeafDescriptor>& descriptors,
    const RzfpFileHeader& header,
    const ERWT3DHeader& planHeader,
    const RzfpReaderConfig& config,
    RzfpReadProfile& profile,
    const PrefixCheckpointMap& checkpoints
) {
    const uint64_t leavesPerSB = rzfpTotalLeafsPerSuper(header);
    const auto groups = groupTasksBySuperblock(tasks);

    std::vector<ReadInterval> intervals;
    intervals.reserve(sbIndex.size());
    for (uint64_t sbid = 0; sbid < sbIndex.size(); ++sbid) {
        intervals.push_back({
            sbIndex[sbid].payload_offset,
            sbIndex[sbid].payload_bytes,
            sbid
        });
    }
    for (const auto& task : tasks) {
        profile.requested_record_bytes += task.record_size;
    }

    const auto decode = [&](
        uint64_t sbid,
        const uint8_t* payload,
        RzfpCodec& codec
    ) -> bool {
        const auto groupIt = groups.find(sbid);
        if (groupIt == groups.end()) return true;

        const uint64_t descriptorBase = sbid * leavesPerSB;
        const auto& checkpoint = checkpoints.at(sbid);
        bool ok = true;

        for (size_t taskIndex : groupIt->second) {
            const auto& task = tasks[taskIndex];
            const uint32_t offset = prefixFromCheckpoint(
                task.morton,
                checkpoint,
                descriptors,
                descriptorBase
            );
            float leaf[64];
            if (!codec.decodeRecord(
                    task.codec,
                    payload + offset,
                    task.record_size,
                    leaf)) {
                std::cerr << "Error: RZFP decode failed for sb="
                          << task.physical_sb_id
                          << " morton=" << task.morton << std::endl;
                ok = false;
                continue;
            }

            const auto scatterStart = Clock::now();
            for (const auto& scatter : task.scatters) {
                scatterDecodedLeaf(
                    planHeader,
                    scatter.op,
                    leaf,
                    scatter.output
                );
            }
            profile.scatter_ns.fetch_add(
                nsSince(scatterStart),
                std::memory_order_relaxed
            );
        }

        return ok;
    };

    return executeWindowedRead(fd, intervals, config, decode, profile);
}

#pragma pack(push, 1)
struct XPlaneHeader {
    char magic[8];
    uint64_t version;
    uint64_t nx;
    uint64_t ny;
    uint64_t nz;
    uint64_t data_offset;
    uint64_t reserved[26];
};

struct XPlaneIndexEntry {
    uint64_t offset;
    uint32_t size;
    uint32_t reserved;
};
#pragma pack(pop)

static bool magicMatches(const char* a, const char* b) {
    return std::memcmp(a, b, 8) == 0;
}

} // namespace

RzfpReader::RzfpReader(const std::string& path)
    : path_(path), fd_(-1) {
    fd_ = open(path.c_str(), O_RDONLY);
    if (fd_ < 0) return;

    struct stat st{};
    if (fstat(fd_, &st) != 0) {
        close(fd_);
        fd_ = -1;
        return;
    }

    file_size_ = static_cast<uint64_t>(st.st_size);
    file_identity_ = combineFileIdentity(st, file_size_);

    if (!readFullyAt(fd_, &header_, sizeof(header_), 0)) {
        close(fd_);
        fd_ = -1;
        return;
    }

    if (!validateRzfpHeader(header_)) {
        close(fd_);
        fd_ = -1;
        return;
    }

    const uint64_t totalSB = rzfpTotalSuperblocks(header_);
    const uint64_t totalLeaves = rzfpTotalLeaves(header_);
    const uint64_t indexBytes =
        totalSB * sizeof(RzfpSuperblockIndex);
    const uint64_t descriptorBytes =
        totalLeaves * sizeof(RzfpLeafDescriptor);

    if (sizeof(RzfpFileHeader) > file_size_ ||
        indexBytes > file_size_ - sizeof(RzfpFileHeader)) {
        close(fd_);
        fd_ = -1;
        return;
    }
    if (header_.descriptor_offset <
            sizeof(RzfpFileHeader) + indexBytes ||
        descriptorBytes > file_size_ - header_.descriptor_offset) {
        close(fd_);
        fd_ = -1;
        return;
    }
    if (header_.payload_offset <
            header_.descriptor_offset + descriptorBytes ||
        header_.payload_offset > file_size_) {
        close(fd_);
        fd_ = -1;
        return;
    }

    sb_index_.resize(totalSB);
    if (!readFullyAt(
            fd_,
            sb_index_.data(),
            indexBytes,
            sizeof(RzfpFileHeader))) {
        close(fd_);
        fd_ = -1;
        return;
    }

    descriptors_.resize(totalLeaves);
    if (!readFullyAt(
            fd_,
            descriptors_.data(),
            descriptorBytes,
            header_.descriptor_offset)) {
        close(fd_);
        fd_ = -1;
        return;
    }

    for (uint64_t i = 0; i < totalSB; ++i) {
        const auto& index = sb_index_[i];
        if (index.payload_offset < header_.payload_offset ||
            index.payload_offset > file_size_ ||
            index.payload_bytes > file_size_ - index.payload_offset) {
            close(fd_);
            fd_ = -1;
            return;
        }
    }

    for (uint64_t i = 0; i < totalLeaves; ++i) {
        if (static_cast<uint8_t>(
                descriptorCodec(descriptors_[i])) > 5) {
            close(fd_);
            fd_ = -1;
            return;
        }
    }

    for (uint64_t sb = 0; sb < totalSB; ++sb) {
        const uint64_t leafStart =
            sb * rzfpTotalLeafsPerSuper(header_);
        uint64_t sumSizes = 0;
        for (uint64_t j = 0;
             j < rzfpTotalLeafsPerSuper(header_);
             ++j) {
            const uint64_t leafIndex = leafStart + j;
            if (leafIndex >= totalLeaves) break;
            sumSizes += descriptorSize(descriptors_[leafIndex]);
        }
        if (sumSizes != sb_index_[sb].payload_bytes) {
            close(fd_);
            fd_ = -1;
            return;
        }
    }

    payload_bytes_ = totalPayloadBytes(sb_index_);
    openAxisSidecars_();
    initRawXAux_();
}

RzfpReader::~RzfpReader() {
    if (rawXAuxFd_ >= 0) close(rawXAuxFd_);
    if (fd_ >= 0) close(fd_);
    for (int i = 0; i < 3; ++i) {
        if (sidecar_fd_[i] >= 0 && sidecar_fd_[i] != fd_) close(sidecar_fd_[i]);
    }
}

const DeviceProfile& RzfpReader::ensureDeviceProfile(
    const DeviceCalibrationConfig& config
) {
    if (fd_ >= 0 && !device_profile_ready_) {
        device_profile_ = DeviceProfileCache::instance().getOrCalibrate(
            fd_,
            file_size_,
            config
        );
        device_profile_ready_ = true;
    }
    return device_profile_;
}

bool RzfpReader::dropPayloadCache() {
    if (fd_ < 0 || sb_index_.empty()) return false;

    uint64_t start = std::numeric_limits<uint64_t>::max();
    uint64_t end = 0;
    for (const auto& sb : sb_index_) {
        start = std::min(start, sb.payload_offset);
        uint64_t currentEnd = 0;
        if (checkedAddU64(sb.payload_offset, sb.payload_bytes, currentEnd)) {
            end = std::max(end, currentEnd);
        }
    }

    if (start >= end) return false;
    return posix_fadvise(
        fd_,
        static_cast<off_t>(start),
        static_cast<off_t>(end - start),
        POSIX_FADV_DONTNEED
    ) == 0;
}

void RzfpReader::initRawXAux_() {
    if (!hasRawXAux(header_)) return;

    RawXAuxRegion region;
    region.offset = rzfpRawXAuxOffset(header_);
    region.bytes = rzfpRawXAuxBytes(header_);
    region.plane_bytes = rzfpRawXAuxPlaneBytes(header_);
    region.version = rzfpRawXAuxVersion(header_);

    uint64_t minimumOffset = header_.payload_offset;
    for (const auto& sb : sb_index_) {
        uint64_t end = 0;
        if (checkedAddU64(sb.payload_offset, sb.payload_bytes, end)) {
            minimumOffset = std::max(minimumOffset, end);
        }
    }

    const auto error = validateRawXAuxRegion(
        file_size_,
        minimumOffset,
        header_.nx,
        header_.ny,
        header_.nz,
        region
    );
    if (error != RawXAuxValidationError::None) {
        std::cerr << "Warning: RZFP Raw X auxiliary validation failed: "
                  << rawXAuxValidationErrorStr(error)
                  << " — falling back to main file reader" << std::endl;
        return;
    }

    rawXAuxOffset_ = region.offset;
    rawXAuxPlaneBytes_ = region.plane_bytes;
    rawXAuxFd_ = open(path_.c_str(), O_RDONLY);
    if (rawXAuxFd_ < 0) return;
    rawXAuxAvailable_ = true;
}

bool RzfpReader::tryReadSliceRawXAux_(
    uint64_t x,
    float* output,
    RzfpReadProfile* profile
) {
    if (!rawXAuxAvailable_ || x >= header_.nx) return false;

    const uint64_t planeBytes =
        header_.ny * header_.nz * sizeof(float);
    const uint64_t offset =
        rawXAuxOffset_ + x * rawXAuxPlaneBytes_;

    const auto t0 = Clock::now();
    if (!readFullyAt(rawXAuxFd_, output, planeBytes, offset)) {
        return false;
    }

    if (profile) {
        profile->io_time_ms += msSince(t0);
        ++profile->pread_calls;
        profile->actual_read_bytes += planeBytes;
        profile->requested_record_bytes += planeBytes;
        profile->selected_strategy = RzfpReadStrategy::RawXAux;
        profile->strategy_reason = "direct Raw X auxiliary plane";
    }

    (void)posix_fadvise(
        rawXAuxFd_,
        static_cast<off_t>(offset),
        static_cast<off_t>(planeBytes),
        POSIX_FADV_DONTNEED
    );
    return true;
}

bool RzfpReader::tryReadBatchRawXAux_(
    const std::vector<SliceBatchRequest>& requests,
    const RzfpReaderConfig&,
    RzfpReadProfile& profile
) {
    if (!rawXAuxAvailable_ || requests.empty()) return false;

    struct RawXAuxTask {
        uint64_t file_offset = 0;
        float* output = nullptr;
    };

    std::vector<RawXAuxTask> tasks;
    tasks.reserve(requests.size());
    for (const auto& request : requests) {
        if (request.axis != SliceAxis::X ||
            request.index >= header_.nx) {
            continue;
        }
        tasks.push_back({
            rawXAuxOffset_ +
                request.index * rawXAuxPlaneBytes_,
            request.output
        });
    }
    if (tasks.empty()) return false;

    const uint64_t planeBytes =
        header_.ny * header_.nz * sizeof(float);
    for (size_t i = 0; i < tasks.size(); ++i) {
        profile.requested_record_bytes += planeBytes;
    }

    std::sort(
        tasks.begin(),
        tasks.end(),
        [](const RawXAuxTask& a, const RawXAuxTask& b) {
            return a.file_offset < b.file_offset;
        }
    );

    constexpr uint64_t MAX_WINDOW = 256ULL * MiB;
    std::vector<uint8_t> window;
    size_t i = 0;

    while (i < tasks.size()) {
        const uint64_t windowStart = tasks[i].file_offset;
        uint64_t windowEnd = windowStart + planeBytes;
        size_t j = i + 1;
        while (j < tasks.size()) {
            const uint64_t nextOffset = tasks[j].file_offset;
            if (nextOffset > windowEnd + planeBytes) break;
            const uint64_t proposedEnd = nextOffset + planeBytes;
            if (proposedEnd - windowStart > MAX_WINDOW) break;
            windowEnd = proposedEnd;
            ++j;
        }

        const uint64_t windowSize = windowEnd - windowStart;
        window.resize(static_cast<size_t>(windowSize));
        const auto t0 = Clock::now();
        if (!readFullyAt(
                rawXAuxFd_,
                window.data(),
                windowSize,
                windowStart)) {
            return false;
        }
        profile.io_time_ms += msSince(t0);
        ++profile.pread_calls;
        profile.actual_read_bytes += windowSize;

        for (size_t k = i; k < j; ++k) {
            const uint64_t offset =
                tasks[k].file_offset - windowStart;
            std::memcpy(
                tasks[k].output,
                window.data() + offset,
                static_cast<size_t>(planeBytes)
            );
        }

        (void)posix_fadvise(
            rawXAuxFd_,
            static_cast<off_t>(windowStart),
            static_cast<off_t>(windowSize),
            POSIX_FADV_DONTNEED
        );
        i = j;
    }

    profile.selected_strategy = RzfpReadStrategy::RawXAux;
    profile.strategy_reason = "direct Raw X auxiliary batch";
    return true;
}

void RzfpReader::openAxisSidecars_() {
    // Try X sidecar first (v1 legacy + v2)
    {
        const std::string xpPath = path_ + ".xp";
        int fd = open(xpPath.c_str(), O_RDONLY);
        if (fd >= 0) {
            // Try v2 header first
            AxisPlaneHeader v2Hdr{};
            if (readFullyAt(fd, &v2Hdr, sizeof(v2Hdr), 0) &&
                std::memcmp(v2Hdr.magic, AXISPLANE_MAGIC, 8) == 0 &&
                v2Hdr.axis == static_cast<uint8_t>(PlaneAxis::X) &&
                v2Hdr.nx == header_.nx && v2Hdr.ny == header_.ny && v2Hdr.nz == header_.nz) {
                uint64_t pc = v2Hdr.plane_count;
                sidecar_offsets_[0].resize(pc);
                sidecar_sizes_[0].resize(pc);
                std::vector<AxisPlaneIndexEntry> entries(pc);
                if (readFullyAt(fd, entries.data(), pc * sizeof(AxisPlaneIndexEntry), sizeof(AxisPlaneHeader))) {
                    for (uint64_t i = 0; i < pc; ++i) {
                        sidecar_offsets_[0][i] = entries[i].offset;
                        sidecar_sizes_[0][i] = entries[i].compressed_size;
                    }
                    sidecar_fd_[0] = fd;
                    has_sidecar_[0] = true;
                    goto tryY;
                }
            } else {
                // Try legacy v1 header
                XPlaneHeader v1Hdr{};
                lseek(fd, 0, SEEK_SET);
                if (readFullyAt(fd, &v1Hdr, sizeof(v1Hdr), 0)) {
                    const char expectedMagic[8] = {'E','R','W','T','3','D','X',' '};
                    if (magicMatches(v1Hdr.magic, expectedMagic) && v1Hdr.version == 1 &&
                        v1Hdr.nx == header_.nx && v1Hdr.ny == header_.ny && v1Hdr.nz == header_.nz) {
                        uint64_t pc = v1Hdr.nx;
                        sidecar_offsets_[0].resize(pc);
                        sidecar_sizes_[0].resize(pc);
                        std::vector<XPlaneIndexEntry> entries(pc);
                        if (readFullyAt(fd, entries.data(), pc * sizeof(XPlaneIndexEntry), sizeof(XPlaneHeader))) {
                            for (uint64_t i = 0; i < pc; ++i) {
                                sidecar_offsets_[0][i] = entries[i].offset;
                                sidecar_sizes_[0][i] = entries[i].size;
                            }
                            sidecar_fd_[0] = fd;
                            has_sidecar_[0] = true;
                            goto tryY;
                        }
                    }
                }
            }
            close(fd);
        }
    }

tryY:
    // Try Y sidecar (v2 only)
    {
        const std::string ypPath = path_ + ".yp";
        int fd = open(ypPath.c_str(), O_RDONLY);
        if (fd >= 0) {
            AxisPlaneHeader hdr{};
            if (readFullyAt(fd, &hdr, sizeof(hdr), 0) &&
                validateAxisPlaneHeader(hdr, header_.nx, header_.ny, header_.nz) &&
                hdr.axis == static_cast<uint8_t>(PlaneAxis::Y)) {
                uint64_t pc = hdr.plane_count;
                sidecar_offsets_[1].resize(pc);
                sidecar_sizes_[1].resize(pc);
                std::vector<AxisPlaneIndexEntry> entries(pc);
                if (readFullyAt(fd, entries.data(), pc * sizeof(AxisPlaneIndexEntry), sizeof(AxisPlaneHeader))) {
                    for (uint64_t i = 0; i < pc; ++i) {
                        sidecar_offsets_[1][i] = entries[i].offset;
                        sidecar_sizes_[1][i] = entries[i].compressed_size;
                    }
                    sidecar_fd_[1] = fd;
                    has_sidecar_[1] = true;
                    goto tryZ;
                }
            }
            close(fd);
        }
    }

tryZ:
    // Try Z sidecar (v2 only)
    {
        const std::string zpPath = path_ + ".zp";
        int fd = open(zpPath.c_str(), O_RDONLY);
        if (fd >= 0) {
            AxisPlaneHeader hdr{};
            if (readFullyAt(fd, &hdr, sizeof(hdr), 0) &&
                validateAxisPlaneHeader(hdr, header_.nx, header_.ny, header_.nz) &&
                hdr.axis == static_cast<uint8_t>(PlaneAxis::Z)) {
                uint64_t pc = hdr.plane_count;
                sidecar_offsets_[2].resize(pc);
                sidecar_sizes_[2].resize(pc);
                std::vector<AxisPlaneIndexEntry> entries(pc);
                if (readFullyAt(fd, entries.data(), pc * sizeof(AxisPlaneIndexEntry), sizeof(AxisPlaneHeader))) {
                    for (uint64_t i = 0; i < pc; ++i) {
                        sidecar_offsets_[2][i] = entries[i].offset;
                        sidecar_sizes_[2][i] = entries[i].compressed_size;
                    }
                    sidecar_fd_[2] = fd;
                    has_sidecar_[2] = true;
                    return;
                }
            }
            close(fd);
        }
    }
}

bool RzfpReader::readAxisPlanesBatchFromSidecar(
    PlaneAxis axis,
    const std::vector<SliceBatchRequest>& requests,
    const RzfpReaderConfig& config,
    RzfpReadProfile& profile
) {
    const int ai = static_cast<int>(axis);
    if (!has_sidecar_[ai] || sidecar_fd_[ai] < 0 || requests.empty()) return false;

    const auto& offsets = sidecar_offsets_[ai];
    const auto& sizes   = sidecar_sizes_[ai];
    const int fd = sidecar_fd_[ai];

    // Output dimensions for decode and scatter
    uint64_t dimA = 0, dimB = 0;
    switch (axis) {
        case PlaneAxis::X: dimA = header_.ny; dimB = header_.nz; break;
        case PlaneAxis::Y: dimA = header_.nx; dimB = header_.nz; break;
        case PlaneAxis::Z: dimA = header_.nx; dimB = header_.ny; break;
    }

    // Group requests by plane_index to deduplicate
    struct STask {
        uint64_t offset = 0;
        uint32_t size = 0;
        std::vector<float*> outputs;
    };
    std::unordered_map<uint64_t, STask> planeMap;

    for (const auto& request : requests) {
        if (request.index >= offsets.size()) return false;
        auto& task = planeMap[request.index];
        task.offset = offsets[request.index];
        task.size   = sizes[request.index];
        task.outputs.push_back(request.output);
        profile.requested_record_bytes += sizes[request.index];
    }

    std::vector<STask> tasks;
    tasks.reserve(planeMap.size());
    for (auto& kv : planeMap) tasks.push_back(std::move(kv.second));

    std::sort(tasks.begin(), tasks.end(),
              [](const STask& a, const STask& b) {
                  if (a.offset != b.offset) return a.offset < b.offset;
                  return a.size > b.size;
              });

    const uint64_t readWindow = config.hdd.read_window_bytes > 0
        ? config.hdd.read_window_bytes : 512ULL * MiB;
    const uint64_t maxGap = config.hdd.max_gap_bytes > 0
        ? config.hdd.max_gap_bytes : 8ULL * MiB;
    const int decodeThreads = std::max(1, config.decode_threads);

    ThreadPool pool(static_cast<size_t>(decodeThreads), false);
    std::vector<uint8_t> window;
    size_t i = 0;

    while (i < tasks.size()) {
        const uint64_t windowStart = tasks[i].offset;
        uint64_t windowEnd = windowStart + tasks[i].size;
        size_t j = i + 1;
        while (j < tasks.size()) {
            const uint64_t off = tasks[j].offset;
            const uint64_t end = off + tasks[j].size;
            if (off > windowEnd + maxGap) break;
            if (end - windowStart > readWindow) break;
            windowEnd = std::max(windowEnd, end);
            ++j;
        }

        const uint64_t windowSize = windowEnd - windowStart;
        window.resize(static_cast<size_t>(windowSize));
        const auto ioStart = Clock::now();
        if (!readFullyAt(fd, window.data(), windowSize, windowStart)) {
            std::cerr << "Error: sidecar batch read failed offset=" << windowStart << std::endl;
            return false;
        }
        profile.io_time_ms += msSince(ioStart);
        ++profile.pread_calls;
        profile.actual_read_bytes += windowSize;

        const size_t count = j - i;
        std::vector<std::vector<float>> xPlanes;
        if (axis == PlaneAxis::X) {
            xPlanes.assign(
                count,
                std::vector<float>(
                    static_cast<size_t>(dimA * dimB)));
        }

        const int threadsToUse = static_cast<int>(
            std::min<size_t>(decodeThreads, count));
        bool decodeOk = true;
        const auto decodeStart = Clock::now();

        auto decodeTarget = [&](size_t taskIndex) -> float* {
            if (axis == PlaneAxis::X)
                return xPlanes[taskIndex - i].data();
            return tasks[taskIndex].outputs.front();
        };

        if (threadsToUse <= 1) {
            for (size_t k = i; k < j && decodeOk; ++k) {
                if (!decodeXPlane2D(
                        window.data() +
                            (tasks[k].offset - windowStart),
                        tasks[k].size,
                        decodeTarget(k),
                        dimA,
                        dimB)) {
                    decodeOk = false;
                }
            }
        } else {
            std::vector<std::future<bool>> futures;
            const size_t perThread =
                (count + static_cast<size_t>(threadsToUse) - 1) /
                static_cast<size_t>(threadsToUse);
            for (int thread = 0; thread < threadsToUse; ++thread) {
                const size_t start =
                    i + static_cast<size_t>(thread) * perThread;
                const size_t end = std::min(start + perThread, j);
                if (start >= end) break;
                futures.push_back(pool.submit([&, start, end]() {
                    for (size_t k = start; k < end; ++k) {
                        if (!decodeXPlane2D(
                                window.data() +
                                    (tasks[k].offset - windowStart),
                                tasks[k].size,
                                decodeTarget(k),
                                dimA,
                                dimB)) {
                            return false;
                        }
                    }
                    return true;
                }));
            }
            for (auto& future : futures) {
                if (!future.get()) decodeOk = false;
            }
        }

        if (!decodeOk) {
            std::cerr << "Error: sidecar batch decode failed"
                      << std::endl;
            return false;
        }
        profile.decode_time_ms += msSince(decodeStart);

        const auto scatterStart = Clock::now();
        const size_t planeBytes =
            static_cast<size_t>(dimA * dimB) * sizeof(float);
        for (size_t k = i; k < j; ++k) {
            float* const primary = tasks[k].outputs.front();
            if (axis == PlaneAxis::X) {
                const auto& plane = xPlanes[k - i];
                for (uint64_t y = 0; y < header_.ny; ++y) {
                    for (uint64_t z = 0; z < header_.nz; ++z) {
                        primary[y * header_.nz + z] =
                            plane[z * header_.ny + y];
                    }
                }
            }
            for (size_t outputIndex = 1;
                 outputIndex < tasks[k].outputs.size();
                 ++outputIndex) {
                std::memcpy(
                    tasks[k].outputs[outputIndex],
                    primary,
                    planeBytes);
            }
        }
        profile.scatter_time_ms += msSince(scatterStart);
        i = j;
    }

    profile.selected_strategy = RzfpReadStrategy::XPlaneSidecar;
    profile.strategy_reason = "compressed axis-plane sidecar";
    return true;
}

bool RzfpReader::readSlice(
    SliceAxis axis,
    uint64_t index,
    float* output,
    int numThreads,
    size_t memoryLimitMB,
    const HDDReadWindowConfig& windowConfig
) {
    const SliceBatchRequest request{axis, index, output};
    return readSlicesBatch(
        {request},
        numThreads,
        memoryLimitMB,
        windowConfig
    );
}

bool RzfpReader::readSlicesBatch(
    const std::vector<SliceBatchRequest>& requests,
    int numThreads,
    size_t memoryLimitMB,
    const HDDReadWindowConfig& windowConfig
) {
    RzfpReaderConfig config;
    config.hdd = windowConfig;
    config.strategy = RzfpReadStrategy::Auto;
    config.decode_threads = std::max(1, numThreads);

    if (memoryLimitMB > 0 && config.hdd.read_window_bytes == 0) {
        const uint64_t automaticWindow = 512ULL * MiB;
        const uint64_t budgetWindow =
            static_cast<uint64_t>(memoryLimitMB) * MiB / 4;
        config.hdd.read_window_bytes = std::min(
            automaticWindow,
            std::max<uint64_t>(16ULL * MiB, budgetWindow)
        );
    }
    if (memoryLimitMB > 0 && config.hdd.max_gap_bytes == 0) {
        config.hdd.max_gap_bytes = std::max<uint64_t>(
            1ULL * MiB,
            config.hdd.read_window_bytes / 64
        );
    }

    return readSlicesBatch(requests, config);
}

bool RzfpReader::readSlicesBatch(
    const std::vector<SliceBatchRequest>& requests,
    const RzfpReaderConfig& requestedConfig
) {
    if (fd_ < 0 || requests.empty()) return false;

    RzfpReaderConfig config = requestedConfig;
    if (config.window_cache_file_identity == 0) {
        config.window_cache_file_identity = file_identity_;
    }

    RzfpReadProfile localProfile;
    RzfpReadProfile* profile = config.profile
        ? config.profile
        : &localProfile;
    profile->cache_policy = config.adaptive.cache_policy;

    // Cold-group behavior is explicit and local to this call. Production
    // StableAuto and WarmAllowed modes retain both page cache and the bounded
    // user-space cache between groups.
    if (config.adaptive.cache_policy == CachePolicy::DeterministicCold) {
        (void)dropPayloadCache();
        if (config.window_cache) {
            config.window_cache->clear();
        }
    }

    std::vector<SliceBatchRequest> fallback;
    fallback.reserve(requests.size());
    const auto classifyStart = Clock::now();

    if (rawXAuxAvailable_) {
        std::vector<SliceBatchRequest> xRequests;
        xRequests.reserve(requests.size());
        for (const auto& request : requests) {
            if (request.axis == SliceAxis::X) {
                xRequests.push_back(request);
            } else {
                fallback.push_back(request);
            }
        }
        if (!xRequests.empty() &&
            !tryReadBatchRawXAux_(xRequests, config, *profile)) {
            fallback.insert(
                fallback.end(),
                xRequests.begin(),
                xRequests.end()
            );
        }
    } else {
        // Try each axis sidecar individually
        for (int ai = 0; ai < 3; ++ai) {
            if (!has_sidecar_[ai]) continue;
            PlaneAxis paxis = static_cast<PlaneAxis>(ai);
            SliceAxis saxis = axisToSliceAxis(paxis);

            std::vector<SliceBatchRequest> sidecarReqs;
            sidecarReqs.reserve(requests.size());
            for (const auto& request : requests) {
                if (request.axis == saxis) {
                    sidecarReqs.push_back(request);
                }
            }
            if (!sidecarReqs.empty()) {
                if (readAxisPlanesBatchFromSidecar(paxis, sidecarReqs, config, *profile)) {
                    continue; // handled, don't fall back
                }
                // Fall back to main format
                fallback.insert(fallback.end(), sidecarReqs.begin(), sidecarReqs.end());
            }
        }
        // Remaining requests (axes without sidecar) go to fallback
        for (const auto& request : requests) {
            bool handled = false;
            for (int ai = 0; ai < 3; ++ai) {
                if (has_sidecar_[ai] && request.axis == axisToSliceAxis(static_cast<PlaneAxis>(ai))) {
                    handled = true; break;
                }
            }
            if (!handled) fallback.push_back(request);
        }
    }

    profile->plan_time_ms += msSince(classifyStart);
    if (fallback.empty()) return true;

    const ERWT3DHeader planHeader = planHeaderFromRzfp(header_);
    const uint64_t leavesPerSB = rzfpTotalLeafsPerSuper(header_);

    double planTime = 0.0;
    auto tasks = buildLeafTasks(
        planHeader,
        fallback,
        header_,
        planTime,
        profile
    );
    if (tasks.empty()) {
        profile->plan_time_ms += planTime;
        return true;
    }

    double prefixTime = 0.0;
    const auto checkpoints = buildPrefixCheckpoints(
        tasks,
        descriptors_,
        leavesPerSB
    );
    computeTaskOffsets(
        tasks,
        descriptors_,
        sb_index_,
        leavesPerSB,
        checkpoints,
        prefixTime
    );
    std::sort(
        tasks.begin(),
        tasks.end(),
        [](const RzfpLeafTask& a, const RzfpLeafTask& b) {
            return a.file_offset < b.file_offset;
        }
    );

    profile->unique_leaves += tasks.size();
    std::unordered_set<uint64_t> uniqueSuperblocks;
    for (const auto& task : tasks) {
        uniqueSuperblocks.insert(task.physical_sb_id);
    }
    profile->unique_superblocks += uniqueSuperblocks.size();
    profile->plan_time_ms += planTime;
    profile->prefix_time_ms += prefixTime;

    if (config.io_profile == IOProfileType::SSD ||
        config.io_profile == IOProfileType::WSL_SSD) {
        return executeSelectiveLeafSSD(fd_, tasks, planHeader, config, *profile);
    }

    RzfpReadStrategy strategy = config.strategy;
    if (strategy == RzfpReadStrategy::Auto) {
        const DeviceProfile& device = ensureDeviceProfile();
        const StrategyDecision decision = buildAdaptiveDecision(
            fd_,
            tasks,
            sb_index_,
            config,
            device,
            *profile
        );
        strategy = decision.selected;
    } else {
        profile->strategy_reason = "strategy forced by caller";
    }
    profile->selected_strategy = strategy;

    switch (strategy) {
        case RzfpReadStrategy::SelectiveLeaf:
            return executeSelectiveLeaf(
                fd_,
                tasks,
                planHeader,
                config,
                *profile
            );
        case RzfpReadStrategy::WholeSuperblock:
            return executeWholeSuperblock(
                fd_,
                tasks,
                checkpoints,
                descriptors_,
                sb_index_,
                planHeader,
                config,
                *profile,
                leavesPerSB
            );
        case RzfpReadStrategy::FullPayloadScan:
            return executeFullPayloadScan(
                fd_,
                tasks,
                sb_index_,
                descriptors_,
                header_,
                planHeader,
                config,
                *profile,
                checkpoints
            );
        default:
            return executeSelectiveLeaf(
                fd_,
                tasks,
                planHeader,
                config,
                *profile
            );
    }
}

bool RzfpReader::readContestRound(
    const std::vector<ContestRoundGroup>& groups,
    const RzfpReaderConfig& config,
    std::vector<RzfpRoundReadResult>* results
) {
    if (results) {
        results->clear();
        results->resize(groups.size());
    }

    size_t requestCount = 0;
    for (const auto& group : groups) {
        if (group.indices.size() != group.outputs.size()) return false;
        requestCount += group.indices.size();
    }

    std::vector<SliceBatchRequest> requests;
    requests.reserve(requestCount);
    for (const auto& group : groups) {
        for (size_t i = 0; i < group.indices.size(); ++i) {
            requests.push_back(
                {group.axis, group.indices[i], group.outputs[i]});
        }
    }
    if (requests.empty()) return true;

    RzfpReaderConfig roundConfig = config;
    RzfpReadProfile roundProfile;
    roundConfig.profile = &roundProfile;

    const auto readStart = Clock::now();
    if (!readSlicesBatch(requests, roundConfig)) return false;
    const double readTimeMs = msSince(readStart);

    if (results) {
        for (size_t g = 0; g < groups.size(); ++g) {
            RzfpRoundReadResult& result = (*results)[g];
            result.read_time_ms = readTimeMs;
            result.io_time_ms = roundProfile.io_time_ms;
            result.decode_time_ms = roundProfile.decode_time_ms;
            result.scatter_time_ms = roundProfile.scatter_time_ms;
            result.unique_leaves = roundProfile.unique_leaf_requests;
            result.duplicate_leaf_requests =
                roundProfile.duplicate_leaf_requests;
            result.logical_leaf_requests =
                roundProfile.logical_leaf_requests;
            result.planned_read_bytes =
                roundProfile.unique_record_bytes != 0
                    ? roundProfile.unique_record_bytes
                    : roundProfile.actual_read_bytes;
            result.actual_read_bytes = roundProfile.actual_read_bytes;
            result.eliminated_read_bytes =
                roundProfile.eliminated_record_bytes;
            result.read_reduction_ratio =
                roundProfile.dedupReductionRatio();
            result.selected_strategy = roundProfile.selected_strategy;
            result.strategy_reason = roundProfile.strategy_reason;
            result.round_plan_built = true;
            result.round_unique_superblocks =
                roundProfile.unique_superblocks;
            result.round_planned_preads = roundProfile.pread_calls;
            result.codec_profile = roundProfile.codec_profile;
        }
    }

    return true;
}

bool RzfpReader::readFullToFile(
    const std::string& outputPath,
    const RzfpReaderConfig& /*config*/)
{
    if (fd_ < 0) return false;

    uint64_t rawBytes = 0;
    if (!checkedMulU64(header_.nx, header_.ny, rawBytes) ||
        !checkedMulU64(rawBytes, header_.nz, rawBytes) ||
        !checkedMulU64(rawBytes, sizeof(float), rawBytes)) {
        std::cerr << "Error: raw size overflow\n";
        return false;
    }
    const uint64_t totalSB = rzfpTotalSuperblocks(header_);
    const uint64_t leavesPerSB = rzfpTotalLeafsPerSuper(header_);

    int outFd = open(outputPath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (outFd < 0) {
        std::cerr << "Error: cannot create output file " << outputPath << "\n";
        return false;
    }
    if (ftruncate(outFd, static_cast<off_t>(rawBytes)) != 0) {
        std::cerr << "Error: ftruncate failed for " << outputPath
                  << " (" << std::strerror(errno) << ")\n";
        close(outFd);
        std::filesystem::path p(outputPath);
        std::error_code ec;
        (void)std::filesystem::remove(p, ec);
        return false;
    }

    bool useMmap = true;
    void* mmapPtr = mmap(nullptr, static_cast<size_t>(rawBytes),
                          PROT_WRITE, MAP_SHARED, outFd, 0);
    if (mmapPtr == MAP_FAILED) {
        useMmap = false;
    }

    const ERWT3DHeader planHeader = planHeaderFromRzfp(header_);
    const uint64_t sgX = rzfpSuperGridX(header_);

    RzfpCodec codec;
    bool ok = true;

    for (uint64_t sb = 0; sb < totalSB && ok; ++sb) {
        const uint64_t physicalSb = physicalSuperblockId(header_, sb);
        const uint64_t descriptorBase = sb * leavesPerSB;

        const auto& sbIdx = sb_index_[physicalSb];
        const uint64_t payloadOffset = sbIdx.payload_offset;
        const uint64_t payloadSize = sbIdx.payload_bytes;

        std::vector<uint8_t> payload(payloadSize);
        if (!readFullyAt(fd_, payload.data(), payloadSize, payloadOffset)) {
            std::cerr << "Error: failed to read superblock " << sb << " payload\n";
            ok = false;
            break;
        }

        std::vector<uint32_t> leafOffsets(leavesPerSB + 1, 0);
        for (uint64_t i = 0; i < leavesPerSB; ++i) {
            leafOffsets[i + 1] = leafOffsets[i] +
                descriptorSize(descriptors_[descriptorBase + i]);
        }
        if (leafOffsets.back() != payloadSize) {
            std::cerr << "Error: superblock " << sb
                      << " descriptor size mismatch: prefix="
                      << leafOffsets.back() << " payload=" << payloadSize << "\n";
            ok = false;
            break;
        }

        const uint64_t sgY = rzfpSuperGridY(header_);
        const uint64_t sgZ = rzfpSuperGridZ(header_);
        const uint64_t rem = sb / sgX;
        const uint64_t sy = rem % sgY;
        const uint64_t sz = rem / sgY;
        const uint64_t sx = sb % sgX;

        const uint64_t validX = std::min<uint64_t>(header_.super_x, header_.nx - sx * header_.super_x);
        const uint64_t validY = std::min<uint64_t>(header_.super_y, header_.ny - sy * header_.super_y);
        const uint64_t validZ = std::min<uint64_t>(header_.super_z, header_.nz - sz * header_.super_z);
        const uint64_t globalStartX = sx * header_.super_x;
        const uint64_t globalStartY = sy * header_.super_y;
        const uint64_t globalStartZ = sz * header_.super_z;

        for (uint64_t leafIdx = 0; leafIdx < leavesPerSB && ok; ++leafIdx) {
            const auto desc = descriptors_[descriptorBase + leafIdx];
            const uint16_t recordSize = descriptorSize(desc);
            if (recordSize == 0) continue;

            uint32_t lx, ly, lz;
            unmorton3D(static_cast<uint32_t>(leafIdx), lx, ly, lz);

            const uint64_t bx = static_cast<uint64_t>(lx) * header_.leaf_x;
            const uint64_t by = static_cast<uint64_t>(ly) * header_.leaf_y;
            const uint64_t bz = static_cast<uint64_t>(lz) * header_.leaf_z;
            if (bx >= validX || by >= validY || bz >= validZ) continue;

            const uint64_t copyX = std::min<uint64_t>(header_.leaf_x, validX - bx);
            const uint64_t copyY = std::min<uint64_t>(header_.leaf_y, validY - by);
            const uint64_t copyZ = std::min<uint64_t>(header_.leaf_z, validZ - bz);

            const uint32_t offset = leafOffsets[leafIdx];

            const RzfpLeafCodec leafCodec = descriptorCodec(desc);
            float leaf[64];
            if (!codec.decodeRecord(leafCodec, payload.data() + offset, recordSize, leaf)) {
                std::cerr << "Error: decode failed sb=" << sb << " leaf=" << leafIdx << "\n";
                ok = false;
                break;
            }

            const uint64_t ny = header_.ny;
            const uint64_t nz = header_.nz;

            if (useMmap) {
                float* dst = static_cast<float*>(mmapPtr);
                for (uint64_t z = 0; z < copyZ; ++z) {
                    for (uint64_t y = 0; y < copyY; ++y) {
                        for (uint64_t x = 0; x < copyX; ++x) {
                            const uint64_t leafInternal = (z * header_.leaf_y + y) * header_.leaf_x + x;
                            const uint64_t gx = globalStartX + bx + x;
                            const uint64_t gy = globalStartY + by + y;
                            const uint64_t gz = globalStartZ + bz + z;
                            const uint64_t rawOff = ((gx * ny + gy) * nz + gz);
                            dst[rawOff] = leaf[leafInternal];
                        }
                    }
                }
            } else {
                for (uint64_t z = 0; z < copyZ; ++z) {
                    for (uint64_t y = 0; y < copyY; ++y) {
                        for (uint64_t x = 0; x < copyX; ++x) {
                            const uint64_t leafInternal =
                                (z * header_.leaf_y + y) * header_.leaf_x + x;
                            const uint64_t gx = globalStartX + bx + x;
                            const uint64_t gy = globalStartY + by + y;
                            const uint64_t gz = globalStartZ + bz + z;
                            const uint64_t rawOff =
                                ((gx * ny + gy) * nz + gz) * sizeof(float);
                            float val = leaf[leafInternal];
                            ssize_t n = pwrite(outFd, &val, sizeof(float),
                                               static_cast<off_t>(rawOff));
                            if (n != sizeof(float)) {
                                std::cerr << "Error: pwrite failed at " << rawOff
                                          << " (" << std::strerror(errno) << ")\n";
                                ok = false;
                                break;
                            }
                        }
                        if (!ok) break;
                    }
                    if (!ok) break;
                }
            }
        }
    }

    if (useMmap) {
        munmap(mmapPtr, static_cast<size_t>(rawBytes));
    }

    if (close(outFd) != 0) {
        std::cerr << "Error: close failed for " << outputPath
                  << " (" << std::strerror(errno) << ")\n";
        ok = false;
    }

    if (!ok) {
        std::filesystem::path p(outputPath);
        std::error_code ec;
        (void)std::filesystem::remove(p, ec);
        return false;
    }

    struct stat st{};
    if (stat(outputPath.c_str(), &st) != 0 ||
        static_cast<uint64_t>(st.st_size) != rawBytes) {
        std::cerr << "Error: output file size mismatch\n";
        std::filesystem::path p(outputPath);
        std::error_code ec;
        (void)std::filesystem::remove(p, ec);
        return false;
    }

    return true;
}

static std::vector<RzfpLeafTask> buildLineTasks(
    const RzfpFileHeader& header,
    const ERWT3DHeader& planHeader,
    SliceAxis axis, uint64_t fixed1, uint64_t fixed2,
    float* output,
    uint64_t leavesPerSB,
    const std::vector<RzfpLeafDescriptor>& descriptors)
{
    std::vector<RzfpLeafTask> tasks;
    const uint64_t sgX = rzfpSuperGridX(header);
    const uint64_t sgY = rzfpSuperGridY(header);

    uint64_t numOutput = 0;
    switch (axis) {
        case SliceAxis::X: numOutput = header.nx; break;
        case SliceAxis::Y: numOutput = header.ny; break;
        case SliceAxis::Z: numOutput = header.nz; break;
    }
    if (numOutput == 0) return tasks;

    std::memset(output, 0, numOutput * sizeof(float));

    const uint64_t leafsPerX = header.super_x / header.leaf_x;
    const uint64_t leafsPerY = header.super_y / header.leaf_y;
    const uint64_t leafsPerZ = header.super_z / header.leaf_z;

    uint64_t fixed_x = 0, fixed_y = 0, fixed_z = 0;
    if (axis == SliceAxis::X)      { fixed_y = fixed1; fixed_z = fixed2; }
    else if (axis == SliceAxis::Y) { fixed_x = fixed1; fixed_z = fixed2; }
    else                           { fixed_x = fixed1; fixed_y = fixed2; }

    if (axis == SliceAxis::X) {
        uint64_t syCoord = fixed_y / header.super_y;
        uint64_t szCoord = fixed_z / header.super_z;
        const uint64_t syStart0 = syCoord * header.super_y;
        const uint64_t szStart0 = szCoord * header.super_z;
        const uint64_t local_y = fixed_y - syStart0;
        const uint64_t local_z = fixed_z - szStart0;
        const uint64_t fixedLy = local_y / header.leaf_y;
        const uint64_t fixedLz = local_z / header.leaf_z;
        const uint8_t vz = static_cast<uint8_t>(local_z % header.leaf_z);
        const uint8_t vy = static_cast<uint8_t>(local_y % header.leaf_y);

        for (uint64_t sxCoord = 0; sxCoord < sgX; ++sxCoord) {
            if (sxCoord * header.super_x >= header.nx) break;
            uint64_t sb = (szCoord * sgY + syCoord) * sgX + sxCoord;
            const uint64_t physicalSb = physicalSuperblockId(header, sb);
            const uint64_t sxStart = sxCoord * header.super_x;

            for (uint64_t lx = 0; lx < leafsPerX; ++lx) {
                uint32_t li = morton3D(static_cast<uint32_t>(lx),
                                        static_cast<uint32_t>(fixedLy),
                                        static_cast<uint32_t>(fixedLz));
                const uint64_t bx = static_cast<uint64_t>(lx) * header.leaf_x;
                if (sxStart + bx >= header.nx) break;

                const uint64_t descriptorIdx = sb * leavesPerSB + li;
                const auto desc = descriptors[descriptorIdx];
                const uint16_t recordSize = descriptorSize(desc);
                if (recordSize == 0) continue;

                RzfpLeafTask task;
                task.physical_sb_id = physicalSb;
                task.morton = static_cast<uint16_t>(li);
                task.codec = descriptorCodec(desc);
                task.record_size = recordSize;

                uint64_t count = 0;
                for (uint64_t off = 0; off < header.leaf_x; ++off) {
                    if (sxStart + bx + off >= header.nx) break;
                    ++count;
                }
                task.scatters.reserve(count);
                for (uint64_t off = 0; off < header.leaf_x; ++off) {
                    uint64_t gx = sxStart + bx + off;
                    if (gx >= header.nx) break;
                    LeafOp voxOp{};
                    voxOp.out_base = static_cast<uint32_t>(gx);
                    voxOp.out_stride = 1;
                    voxOp.param = static_cast<uint8_t>(off);
                    voxOp.v_inner = vy;
                    voxOp.v_outer = vz;
                    voxOp.pad[0] = 0;
                    task.scatters.push_back({voxOp, output});
                }
                tasks.push_back(std::move(task));
            }
        }
    } else if (axis == SliceAxis::Y) {
        uint64_t sxCoord = fixed_x / header.super_x;
        uint64_t szCoord = fixed_z / header.super_z;
        if (sxCoord >= sgX || szCoord >= rzfpSuperGridZ(header)) return tasks;
        const uint64_t sxStart0 = sxCoord * header.super_x;
        const uint64_t szStart0 = szCoord * header.super_z;
        const uint64_t local_x = fixed_x - sxStart0;
        const uint64_t local_z = fixed_z - szStart0;
        const uint64_t fixedLx = local_x / header.leaf_x;
        const uint64_t fixedLz = local_z / header.leaf_z;
        const uint8_t vx = static_cast<uint8_t>(local_x % header.leaf_x);
        const uint8_t vz = static_cast<uint8_t>(local_z % header.leaf_z);

        for (uint64_t syCoord = 0; syCoord < sgY; ++syCoord) {
            if (syCoord * header.super_y >= header.ny) break;
            uint64_t sb = (szCoord * sgY + syCoord) * sgX + sxCoord;
            const uint64_t physicalSb = physicalSuperblockId(header, sb);
            const uint64_t syStart = syCoord * header.super_y;

            for (uint64_t ly = 0; ly < leafsPerY; ++ly) {
                uint32_t li = morton3D(static_cast<uint32_t>(fixedLx),
                                        static_cast<uint32_t>(ly),
                                        static_cast<uint32_t>(fixedLz));
                const uint64_t by = static_cast<uint64_t>(ly) * header.leaf_y;
                if (syStart + by >= header.ny) break;

                const uint64_t descriptorIdx = sb * leavesPerSB + li;
                const auto desc = descriptors[descriptorIdx];
                const uint16_t recordSize = descriptorSize(desc);
                if (recordSize == 0) continue;

                RzfpLeafTask task;
                task.physical_sb_id = physicalSb;
                task.morton = static_cast<uint16_t>(li);
                task.codec = descriptorCodec(desc);
                task.record_size = recordSize;

                uint64_t count = 0;
                for (uint64_t off = 0; off < header.leaf_y; ++off) {
                    if (syStart + by + off >= header.ny) break;
                    ++count;
                }
                task.scatters.reserve(count);
                for (uint64_t off = 0; off < header.leaf_y; ++off) {
                    uint64_t gy = syStart + by + off;
                    if (gy >= header.ny) break;
                    LeafOp voxOp{};
                    voxOp.out_base = static_cast<uint32_t>(gy);
                    voxOp.out_stride = 1;
                    voxOp.param = static_cast<uint8_t>(off);
                    voxOp.v_inner = vx;
                    voxOp.v_outer = vz;
                    voxOp.pad[0] = 1;
                    task.scatters.push_back({voxOp, output});
                }
                tasks.push_back(std::move(task));
            }
        }
    } else {
        uint64_t sxCoord = fixed_x / header.super_x;
        uint64_t syCoord = fixed_y / header.super_y;
        if (sxCoord >= sgX || syCoord >= sgY) return tasks;
        const uint64_t sxStart0 = sxCoord * header.super_x;
        const uint64_t syStart0 = syCoord * header.super_y;
        const uint64_t local_x = fixed_x - sxStart0;
        const uint64_t local_y = fixed_y - syStart0;
        const uint64_t fixedLx = local_x / header.leaf_x;
        const uint64_t fixedLy = local_y / header.leaf_y;
        const uint8_t vx = static_cast<uint8_t>(local_x % header.leaf_x);
        const uint8_t vy = static_cast<uint8_t>(local_y % header.leaf_y);

        for (uint64_t szCoord = 0; szCoord < rzfpSuperGridZ(header); ++szCoord) {
            if (szCoord * header.super_z >= header.nz) break;
            uint64_t sb = (szCoord * sgY + syCoord) * sgX + sxCoord;
            const uint64_t physicalSb = physicalSuperblockId(header, sb);
            const uint64_t szStart = szCoord * header.super_z;

            for (uint64_t lz = 0; lz < leafsPerZ; ++lz) {
                uint32_t li = morton3D(static_cast<uint32_t>(fixedLx),
                                        static_cast<uint32_t>(fixedLy),
                                        static_cast<uint32_t>(lz));
                const uint64_t bz = static_cast<uint64_t>(lz) * header.leaf_z;
                if (szStart + bz >= header.nz) break;

                const uint64_t descriptorIdx = sb * leavesPerSB + li;
                const auto desc = descriptors[descriptorIdx];
                const uint16_t recordSize = descriptorSize(desc);
                if (recordSize == 0) continue;

                RzfpLeafTask task;
                task.physical_sb_id = physicalSb;
                task.morton = static_cast<uint16_t>(li);
                task.codec = descriptorCodec(desc);
                task.record_size = recordSize;

                uint64_t count = 0;
                for (uint64_t off = 0; off < header.leaf_z; ++off) {
                    if (szStart + bz + off >= header.nz) break;
                    ++count;
                }
                task.scatters.reserve(count);
                for (uint64_t off = 0; off < header.leaf_z; ++off) {
                    uint64_t gz = szStart + bz + off;
                    if (gz >= header.nz) break;
                    LeafOp voxOp{};
                    voxOp.out_base = static_cast<uint32_t>(gz);
                    voxOp.out_stride = 1;
                    voxOp.param = static_cast<uint8_t>(off);
                    voxOp.v_inner = vx;
                    voxOp.v_outer = vy;
                    voxOp.pad[0] = 2;
                    task.scatters.push_back({voxOp, output});
                }
                tasks.push_back(std::move(task));
            }
        }
    }

    return tasks;
}

static void scatterLineVoxel(
    const ERWT3DHeader& hdr,
    const LeafOp& op,
    const float decoded_leaf[64],
    float* output)
{
    (void)hdr;
    const uint64_t lx = hdr.leaf_x;
    const uint64_t ly = hdr.leaf_y;
    const int axis = static_cast<int>(op.pad[0]);
    const uint32_t globalIdx = op.out_base;
    const uint8_t off = op.param;

    if (axis == 0) {
        const uint8_t vy = op.v_inner;
        const uint8_t vz = op.v_outer;
        output[globalIdx] = decoded_leaf[(vz * ly + vy) * lx + off];
    } else if (axis == 1) {
        const uint8_t vx = op.v_inner;
        const uint8_t vz = op.v_outer;
        output[globalIdx] = decoded_leaf[(vz * ly + off) * lx + vx];
    } else {
        const uint8_t vx = op.v_inner;
        const uint8_t vy = op.v_outer;
        output[globalIdx] = decoded_leaf[(off * ly + vy) * lx + vx];
    }
}

bool RzfpReader::readLine(
    SliceAxis axis,
    uint64_t fixed1,
    uint64_t fixed2,
    float* output,
    const RzfpReaderConfig& config)
{
    if (fd_ < 0 || output == nullptr) return false;
    if (axis == SliceAxis::X && (fixed1 >= header_.ny || fixed2 >= header_.nz)) return false;
    if (axis == SliceAxis::Y && (fixed1 >= header_.nx || fixed2 >= header_.nz)) return false;
    if (axis == SliceAxis::Z && (fixed1 >= header_.nx || fixed2 >= header_.ny)) return false;

    const ERWT3DHeader planHeader = planHeaderFromRzfp(header_);
    const uint64_t leavesPerSB = rzfpTotalLeafsPerSuper(header_);

    auto tasks = buildLineTasks(header_, planHeader, axis, fixed1, fixed2,
                                output, leavesPerSB, descriptors_);
    if (tasks.empty()) return true;

    auto checkpoints = buildPrefixCheckpoints(tasks, descriptors_, leavesPerSB);
    double prefixTime = 0.0;
    computeTaskOffsets(tasks, descriptors_, sb_index_, leavesPerSB, checkpoints, prefixTime);
    std::sort(tasks.begin(), tasks.end(),
              [](const RzfpLeafTask& a, const RzfpLeafTask& b) {
                  return a.file_offset < b.file_offset;
              });

    RzfpReadProfile profile;
    std::vector<ReadInterval> intervals;
    intervals.reserve(tasks.size());
    for (size_t i = 0; i < tasks.size(); ++i) {
        intervals.push_back({tasks[i].file_offset, tasks[i].record_size, i});
    }

    const auto decode = [&](uint64_t user, const uint8_t* data,
                            RzfpCodec& codec) -> bool {
        const auto& task = tasks[user];
        float leaf[64];
        if (!codec.decodeRecord(task.codec, data, task.record_size, leaf)) {
            return false;
        }
        for (const auto& scatter : task.scatters) {
            scatterLineVoxel(planHeader, scatter.op, leaf, scatter.output);
        }
        return true;
    };

    RzfpReaderConfig localConfig = config;
    if (localConfig.window_cache_file_identity == 0) {
        localConfig.window_cache_file_identity = file_identity_;
    }
    return executeWindowedRead(fd_, intervals, localConfig, decode, profile);
}

bool RzfpReader::readLineX(uint64_t y, uint64_t z, float* output,
                           const RzfpReaderConfig& config) {
    return readLine(SliceAxis::X, y, z, output, config);
}

bool RzfpReader::readLineY(uint64_t x, uint64_t z, float* output,
                           const RzfpReaderConfig& config) {
    return readLine(SliceAxis::Y, x, z, output, config);
}

bool RzfpReader::readLineZ(uint64_t x, uint64_t y, float* output,
                           const RzfpReaderConfig& config) {
    return readLine(SliceAxis::Z, x, y, output, config);
}

void accumulateReadProfile(RzfpReadProfile& total,
                           const RzfpReadProfile& batch) {
    total.unique_superblocks += batch.unique_superblocks;
    total.unique_leaves += batch.unique_leaves;
    total.requested_record_bytes += batch.requested_record_bytes;
    total.actual_read_bytes += batch.actual_read_bytes;
    total.pread_calls += batch.pread_calls;
    total.window_cache_hits += batch.window_cache_hits;
    total.window_cache_misses += batch.window_cache_misses;
    total.window_cache_contained_hits += batch.window_cache_contained_hits;
    total.window_cache_saved_read_bytes += batch.window_cache_saved_read_bytes;
    total.window_cache_resident_bytes = std::max(
        total.window_cache_resident_bytes, batch.window_cache_resident_bytes);
    total.logical_leaf_requests += batch.logical_leaf_requests;
    total.unique_leaf_requests += batch.unique_leaf_requests;
    total.duplicate_leaf_requests += batch.duplicate_leaf_requests;
    total.logical_record_bytes += batch.logical_record_bytes;
    total.unique_record_bytes += batch.unique_record_bytes;
    total.eliminated_record_bytes += batch.eliminated_record_bytes;
    total.plan_time_ms += batch.plan_time_ms;
    total.prefix_time_ms += batch.prefix_time_ms;
    total.io_time_ms += batch.io_time_ms;
    total.decode_time_ms += batch.decode_time_ms;
    total.scatter_time_ms += batch.scatter_time_ms;
    total.sidecar_io_ms += batch.sidecar_io_ms;
    total.sidecar_decode_ms += batch.sidecar_decode_ms;
    if (total.selected_strategy == RzfpReadStrategy::Auto)
        total.selected_strategy = batch.selected_strategy;
    total.strategy_reason = batch.strategy_reason;
    total.effective_device_mb_s = batch.effective_device_mb_s;
    total.pilot_observed_mb_s = batch.pilot_observed_mb_s;
}

} // namespace erwt3d
