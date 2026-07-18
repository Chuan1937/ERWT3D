#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/morton.hpp"
#include "erwt3d/rzfp_codec.hpp"
#include "erwt3d/rzfp_strategy.hpp"
#include "erwt3d/rzfp_xplane_codec.hpp"
#include "erwt3d/sb_plan.hpp"
#include "erwt3d/thread_pool.hpp"
#include "erwt3d/raw_x_aux.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
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
    double& plan_time_ms
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
            windowEnd = end;
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
            return a.offset < b.offset;
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
            windowEnd = end;
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
            return a.offset < b.offset;
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
            windowEnd = end;
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

        if (config.use_window_cache && config.window_cache) {
            std::shared_ptr<const std::vector<uint8_t>> cached;
            if (config.window_cache->get(key, cached)) {
                if (!cached || cached->size() != windowSize) return false;
                destination.resize(static_cast<size_t>(windowSize));
                std::memcpy(
                    destination.data(),
                    cached->data(),
                    static_cast<size_t>(windowSize)
                );
                ++profile.window_cache_hits;
                profile.window_cache_saved_read_bytes += windowSize;
                profile.window_cache_resident_bytes =
                    config.window_cache->residentBytes();
                return true;
            }
            ++profile.window_cache_misses;
        }

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
        if (!codec.decodeRecord(
                task.codec,
                data,
                task.record_size,
                leaf)) {
            std::cerr << "Error: RZFP decode failed for sb="
                      << task.physical_sb_id
                      << " morton=" << task.morton << std::endl;
            return false;
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
        return true;
    };

    return executeWindowedRead(fd, intervals, config, decode, profile);
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
    openXPlaneSidecar();
    initRawXAux_();
}

RzfpReader::~RzfpReader() {
    if (rawXAuxFd_ >= 0) close(rawXAuxFd_);
    if (fd_ >= 0) close(fd_);
    if (xplane_fd_ >= 0) close(xplane_fd_);
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

bool RzfpReader::openXPlaneSidecar() {
    const std::string sidecarPath = path_ + ".xp";
    const int fd = open(sidecarPath.c_str(), O_RDONLY);
    if (fd < 0) return false;

    XPlaneHeader sidecarHeader{};
    if (!readFullyAt(fd, &sidecarHeader, sizeof(sidecarHeader), 0)) {
        close(fd);
        return false;
    }

    const char expectedMagic[8] = {
        'E', 'R', 'W', 'T', '3', 'D', 'X', ' '
    };
    if (!magicMatches(sidecarHeader.magic, expectedMagic) ||
        sidecarHeader.version != 1 ||
        sidecarHeader.nx != header_.nx ||
        sidecarHeader.ny != header_.ny ||
        sidecarHeader.nz != header_.nz) {
        close(fd);
        return false;
    }

    xplane_offsets_.resize(sidecarHeader.nx);
    xplane_sizes_.resize(sidecarHeader.nx);
    std::vector<XPlaneIndexEntry> entries(sidecarHeader.nx);
    const uint64_t indexBytes =
        sidecarHeader.nx * sizeof(XPlaneIndexEntry);
    if (!readFullyAt(
            fd,
            entries.data(),
            indexBytes,
            sizeof(XPlaneHeader))) {
        close(fd);
        return false;
    }

    struct stat st{};
    uint64_t sidecarSize = 0;
    if (fstat(fd, &st) == 0) {
        sidecarSize = static_cast<uint64_t>(st.st_size);
    }

    for (uint64_t i = 0; i < sidecarHeader.nx; ++i) {
        const auto& entry = entries[i];
        if (sidecarSize > 0) {
            if (entry.size == 0 || entry.size > 512ULL * MiB ||
                entry.offset > sidecarSize ||
                entry.size > sidecarSize - entry.offset) {
                close(fd);
                return false;
            }
        }
        xplane_offsets_[i] = entry.offset;
        xplane_sizes_[i] = entry.size;
    }

    xplane_fd_ = fd;
    has_xplane_ = true;
    return true;
}

bool RzfpReader::readXPlaneFromSidecar(
    uint64_t x,
    float* output,
    RzfpReadProfile* profile
) {
    if (!has_xplane_ || x >= xplane_offsets_.size()) return false;

    const uint64_t offset = xplane_offsets_[x];
    const uint32_t size = xplane_sizes_[x];
    std::vector<uint8_t> record(size);

    const auto ioStart = Clock::now();
    if (!readFullyAt(xplane_fd_, record.data(), size, offset)) {
        return false;
    }
    if (profile) {
        profile->io_time_ms += msSince(ioStart);
        ++profile->pread_calls;
        profile->actual_read_bytes += size;
        profile->requested_record_bytes += size;
    }

    const auto decodeStart = Clock::now();
    std::vector<float> plane(header_.ny * header_.nz);
    const bool ok = decodeXPlane2D(
        record.data(),
        size,
        plane.data(),
        header_.ny,
        header_.nz
    );
    if (profile) {
        profile->decode_time_ms += msSince(decodeStart);
    }
    if (!ok) return false;

    for (uint64_t y = 0; y < header_.ny; ++y) {
        for (uint64_t z = 0; z < header_.nz; ++z) {
            output[y * header_.nz + z] =
                plane[z * header_.ny + y];
        }
    }

    if (profile) {
        profile->selected_strategy = RzfpReadStrategy::XPlaneSidecar;
        profile->strategy_reason = "compressed X-plane sidecar";
    }
    return true;
}

bool RzfpReader::readXPlanesBatchFromSidecar(
    const std::vector<SliceBatchRequest>& requests,
    const RzfpReaderConfig& config,
    RzfpReadProfile& profile
) {
    if (xplane_fd_ < 0 || requests.empty()) return false;

    struct XTask {
        uint64_t offset = 0;
        uint32_t size = 0;
        float* output = nullptr;
    };

    std::vector<XTask> tasks;
    tasks.reserve(requests.size());
    for (const auto& request : requests) {
        if (request.index >= xplane_offsets_.size()) return false;
        tasks.push_back({
            xplane_offsets_[request.index],
            xplane_sizes_[request.index],
            request.output
        });
        profile.requested_record_bytes +=
            xplane_sizes_[request.index];
    }

    std::sort(
        tasks.begin(),
        tasks.end(),
        [](const XTask& a, const XTask& b) {
            return a.offset < b.offset;
        }
    );

    const uint64_t readWindow = config.hdd.read_window_bytes > 0
        ? config.hdd.read_window_bytes
        : 512ULL * MiB;
    const uint64_t maxGap = config.hdd.max_gap_bytes > 0
        ? config.hdd.max_gap_bytes
        : 8ULL * MiB;
    const int decodeThreads = std::max(1, config.decode_threads);

    ThreadPool pool(static_cast<size_t>(decodeThreads), false);
    std::vector<uint8_t> window;
    size_t i = 0;

    while (i < tasks.size()) {
        const uint64_t windowStart = tasks[i].offset;
        uint64_t windowEnd = windowStart + tasks[i].size;
        size_t j = i + 1;
        while (j < tasks.size()) {
            const uint64_t offset = tasks[j].offset;
            const uint64_t end = offset + tasks[j].size;
            if (offset > windowEnd + maxGap) break;
            if (end - windowStart > readWindow) break;
            windowEnd = end;
            ++j;
        }

        const uint64_t windowSize = windowEnd - windowStart;
        window.resize(static_cast<size_t>(windowSize));
        const auto ioStart = Clock::now();
        if (!readFullyAt(
                xplane_fd_,
                window.data(),
                windowSize,
                windowStart)) {
            std::cerr << "Error: RZFP sidecar batch read failed at offset "
                      << windowStart << std::endl;
            return false;
        }
        profile.io_time_ms += msSince(ioStart);
        ++profile.pread_calls;
        profile.actual_read_bytes += windowSize;

        const size_t count = j - i;
        std::vector<std::vector<float>> planes(
            count,
            std::vector<float>(header_.ny * header_.nz)
        );
        const int threadsToUse = static_cast<int>(
            std::min<size_t>(decodeThreads, count)
        );
        bool decodeOk = true;
        const auto decodeStart = Clock::now();

        if (threadsToUse <= 1) {
            for (size_t k = i; k < j; ++k) {
                const auto& task = tasks[k];
                if (!decodeXPlane2D(
                        window.data() + (task.offset - windowStart),
                        task.size,
                        planes[k - i].data(),
                        header_.ny,
                        header_.nz)) {
                    decodeOk = false;
                    break;
                }
            }
        } else {
            std::vector<std::future<bool>> futures;
            const size_t perThread =
                (count + static_cast<size_t>(threadsToUse) - 1) /
                static_cast<size_t>(threadsToUse);
            for (int t = 0; t < threadsToUse; ++t) {
                const size_t start =
                    i + static_cast<size_t>(t) * perThread;
                const size_t end = std::min(start + perThread, j);
                if (start >= end) break;
                futures.push_back(pool.submit([&, start, end]() {
                    bool ok = true;
                    for (size_t k = start; k < end && ok; ++k) {
                        const auto& task = tasks[k];
                        ok = decodeXPlane2D(
                            window.data() +
                                (task.offset - windowStart),
                            task.size,
                            planes[k - i].data(),
                            header_.ny,
                            header_.nz
                        );
                    }
                    return ok;
                }));
            }
            for (auto& future : futures) {
                if (!future.get()) decodeOk = false;
            }
        }

        if (!decodeOk) {
            std::cerr << "Error: RZFP sidecar batch decode failed"
                      << std::endl;
            return false;
        }

        for (size_t k = i; k < j; ++k) {
            const auto& task = tasks[k];
            const auto& plane = planes[k - i];
            for (uint64_t y = 0; y < header_.ny; ++y) {
                for (uint64_t z = 0; z < header_.nz; ++z) {
                    task.output[y * header_.nz + z] =
                        plane[z * header_.ny + y];
                }
            }
        }
        profile.decode_time_ms += msSince(decodeStart);
        i = j;
    }

    profile.selected_strategy = RzfpReadStrategy::XPlaneSidecar;
    profile.strategy_reason = "compressed X-plane sidecar batch";
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
        std::vector<SliceBatchRequest> xRequests;
        xRequests.reserve(requests.size());
        for (const auto& request : requests) {
            if (has_xplane_ && request.axis == SliceAxis::X) {
                xRequests.push_back(request);
            } else {
                fallback.push_back(request);
            }
        }
        if (!xRequests.empty() &&
            !readXPlanesBatchFromSidecar(
                xRequests,
                config,
                *profile)) {
            fallback.insert(
                fallback.end(),
                xRequests.begin(),
                xRequests.end()
            );
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
        planTime
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

    std::vector<SliceBatchRequest> yz_requests;

    for (size_t g = 0; g < groups.size(); ++g) {
        const auto& group = groups[g];
        if (group.axis == SliceAxis::X) {
            RzfpReaderConfig xConfig = config;
            RzfpReadProfile xProfile;
            xConfig.profile = &xProfile;

            std::vector<SliceBatchRequest> xRequests;
            xRequests.reserve(group.indices.size());
            for (size_t i = 0; i < group.indices.size(); ++i) {
                xRequests.push_back({group.axis, group.indices[i], group.outputs[i]});
            }

            const auto t0 = Clock::now();
            if (!readSlicesBatch(xRequests, xConfig)) {
                return false;
            }
            const double readMs = msSince(t0);

            if (results) {
                RzfpRoundReadResult& r = (*results)[g];
                r.read_time_ms = readMs;
                r.io_time_ms = xProfile.io_time_ms;
                r.decode_time_ms = xProfile.decode_time_ms;
                r.scatter_time_ms = xProfile.scatter_time_ms;
                r.unique_leaves = xProfile.unique_leaves;
                r.duplicate_leaf_requests = 0;
                r.logical_leaf_requests = xRequests.size();
                r.planned_read_bytes = xProfile.actual_read_bytes;
                r.actual_read_bytes = xProfile.actual_read_bytes;
                r.eliminated_read_bytes = 0;
                r.selected_strategy = xProfile.selected_strategy;
                r.strategy_reason = xProfile.strategy_reason;
            }
        } else {
            for (size_t i = 0; i < group.indices.size(); ++i) {
                yz_requests.push_back({group.axis, group.indices[i], group.outputs[i]});
            }
        }
    }

    if (yz_requests.empty()) return true;

    RzfpReaderConfig yzConfig = config;
    RzfpReadProfile yzProfile;
    yzConfig.profile = &yzProfile;

    const auto t0 = Clock::now();
    if (!readSlicesBatch(yz_requests, yzConfig)) {
        return false;
    }
    const double totalReadMs = msSince(t0);

    if (results) {
        for (size_t g = 0; g < groups.size(); ++g) {
            const auto& group = groups[g];
            if (group.axis == SliceAxis::X) continue;

            RzfpRoundReadResult& r = (*results)[g];
            r.read_time_ms = totalReadMs;
            r.io_time_ms = yzProfile.io_time_ms;
            r.decode_time_ms = yzProfile.decode_time_ms;
            r.scatter_time_ms = yzProfile.scatter_time_ms;
            r.unique_leaves = yzProfile.unique_leaves;
            r.duplicate_leaf_requests = 0;
            r.logical_leaf_requests = yz_requests.size();
            r.planned_read_bytes = yzProfile.requested_record_bytes;
            r.actual_read_bytes = yzProfile.actual_read_bytes;
            r.eliminated_read_bytes = 0;
            r.selected_strategy = yzProfile.selected_strategy;
            r.strategy_reason = yzProfile.strategy_reason;
        }
    }

    return true;
}

} // namespace erwt3d
