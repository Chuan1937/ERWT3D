#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/morton.hpp"
#include "erwt3d/rzfp_codec.hpp"
#include "erwt3d/sb_plan.hpp"
#include "erwt3d/thread_pool.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace erwt3d {

namespace {

using Clock = std::chrono::high_resolution_clock;

static double msSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

static bool readFullyAt(int fd, void* buffer, size_t bytes, uint64_t offset) {
    auto* dst = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    while (done < bytes) {
        ssize_t n = pread(fd, dst + done, bytes - done, static_cast<off_t>(offset + done));
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return true;
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
    return rzfpSuperblockId(rh, sz, sy, sx,
                            (rh.flags & FLAG_PHYSICAL_ORDER_YZX) ? PhysicalOrder::V05_YZX : PhysicalOrder::ZYX);
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
    uint64_t user = 0; // leaf task index for Selective; sb_id for Whole/Full
};

using DecodeCallback = std::function<void(uint64_t user, const uint8_t* data, RzfpCodec& codec)>;

static std::vector<RzfpLeafTask> buildLeafTasks(
    const ERWT3DHeader& plan_hdr,
    const std::vector<RzfpReader::SliceBatchRequest>& requests,
    const RzfpFileHeader& header,
    double& plan_time_ms
) {
    auto t0 = Clock::now();

    std::vector<SBTaskPlan> plans;
    std::vector<float*> outputs;
    plans.reserve(requests.size());
    outputs.reserve(requests.size());

    for (const auto& r : requests) {
        switch (r.axis) {
            case SliceAxis::X: plans.push_back(buildSBPlanX(plan_hdr, r.index)); break;
            case SliceAxis::Y: plans.push_back(buildSBPlanY(plan_hdr, r.index)); break;
            case SliceAxis::Z: plans.push_back(buildSBPlanZ(plan_hdr, r.index)); break;
        }
        outputs.push_back(r.output);
    }

    std::vector<RzfpLeafTask> tasks;
    tasks.reserve(1024);
    std::unordered_map<uint64_t, size_t> task_map;
    task_map.reserve(1024);

    for (size_t p = 0; p < plans.size(); ++p) {
        const auto& plan = plans[p];
        float* out = outputs[p];
        for (const auto& task : plan.tasks) {
            const uint64_t phys_sb = physicalSuperblockId(header, task.sb_index);
            const LeafOp* ops = plan.leaf_ops.data() + task.first_leaf;
            for (uint32_t li = 0; li < task.leaf_count; ++li) {
                const LeafOp& op = ops[li];
                const uint64_t key = (phys_sb << 16) | op.morton;
                auto it = task_map.find(key);
                if (it == task_map.end()) {
                    RzfpLeafTask lt;
                    lt.physical_sb_id = phys_sb;
                    lt.morton = op.morton;
                    lt.scatters.push_back({op, out});
                    const size_t idx = tasks.size();
                    tasks.push_back(std::move(lt));
                    task_map.emplace(key, idx);
                } else {
                    tasks[it->second].scatters.push_back({op, out});
                }
            }
        }
    }

    plan_time_ms = msSince(t0);
    return tasks;
}

static std::unordered_map<uint64_t, std::vector<uint32_t>> buildPrefixes(
    const std::vector<RzfpLeafTask>& tasks,
    const std::vector<RzfpLeafDescriptor>& descriptors,
    uint64_t leavesPerSB
) {
    std::unordered_map<uint64_t, std::vector<uint32_t>> prefixes;
    prefixes.reserve(tasks.size());
    for (const auto& t : tasks) {
        if (prefixes.find(t.physical_sb_id) != prefixes.end()) continue;
        std::vector<uint32_t> prefix(leavesPerSB + 1, 0);
        const uint64_t descBase = t.physical_sb_id * leavesPerSB;
        for (uint64_t i = 0; i < leavesPerSB; ++i) {
            prefix[i + 1] = prefix[i] + descriptorSize(descriptors[descBase + i]);
        }
        prefixes.emplace(t.physical_sb_id, std::move(prefix));
    }
    return prefixes;
}

static void computeTaskOffsets(
    std::vector<RzfpLeafTask>& tasks,
    const std::vector<RzfpLeafDescriptor>& descriptors,
    const std::vector<RzfpSuperblockIndex>& sb_index,
    uint64_t leavesPerSB,
    double& prefix_time_ms
) {
    auto t0 = Clock::now();
    auto prefixes = buildPrefixes(tasks, descriptors, leavesPerSB);

    for (auto& t : tasks) {
        const auto& prefix = prefixes[t.physical_sb_id];
        const uint64_t descBase = t.physical_sb_id * leavesPerSB;
        const auto descriptor = descriptors[descBase + t.morton];
        t.codec = descriptorCodec(descriptor);
        t.record_size = descriptorSize(descriptor);
        t.file_offset = sb_index[t.physical_sb_id].payload_offset + prefix[t.morton];
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
    uint64_t read_window,
    uint64_t max_gap,
    uint64_t& pread_calls
) {
    pread_calls = 0;
    uint64_t bytes = 0;
    size_t i = 0;
    while (i < tasks.size()) {
        uint64_t wstart = tasks[i].file_offset;
        uint64_t wend = wstart + tasks[i].record_size;
        size_t j = i + 1;
        while (j < tasks.size()) {
            const uint64_t off = tasks[j].file_offset;
            const uint64_t end = off + tasks[j].record_size;
            if (off > wend + max_gap) break;
            if (end - wstart > read_window) break;
            wend = end;
            ++j;
        }
        bytes += wend - wstart;
        ++pread_calls;
        i = j;
    }
    return bytes;
}

static uint64_t totalPayloadBytes(const std::vector<RzfpSuperblockIndex>& sb_index) {
    uint64_t total = 0;
    for (const auto& sb : sb_index) total += sb.payload_bytes;
    return total;
}

static RzfpReadStrategy chooseStrategy(
    const std::vector<RzfpLeafTask>& tasks,
    const std::vector<RzfpSuperblockIndex>& sb_index,
    const RzfpReaderConfig& config,
    uint64_t leavesPerSB
) {
    std::unordered_set<uint64_t> touched_sbs;
    for (const auto& t : tasks) touched_sbs.insert(t.physical_sb_id);

    const uint64_t read_window = config.hdd.read_window_bytes > 0
                                     ? config.hdd.read_window_bytes
                                     : (16ULL * 1024 * 1024);
    const uint64_t max_gap = config.hdd.max_gap_bytes;

    uint64_t selective_preads = 0;
    const uint64_t selective_bytes = estimateSelectiveBytes(tasks, read_window, max_gap, selective_preads);

    uint64_t whole_sb_bytes = 0;
    for (uint64_t sbid : touched_sbs) whole_sb_bytes += sb_index[sbid].payload_bytes;

    const uint64_t fullscan_bytes = totalPayloadBytes(sb_index);
    const uint64_t totalLeaves = sb_index.size() * leavesPerSB;

    // Heuristic auto selector.  Selective is the safest default because it
    // avoids reading untouched superblocks.  Full scan only pays off when the
    // requested leaves already cover a large fraction of the volume and the
    // selective window would still drag in nearly the whole payload.
    const double leaf_coverage = totalLeaves > 0
                                     ? static_cast<double>(tasks.size()) / static_cast<double>(totalLeaves)
                                     : 1.0;

    if (selective_bytes <= fullscan_bytes * 1.05) {
        return RzfpReadStrategy::SelectiveLeaf;
    }

    if (leaf_coverage > 0.30 && fullscan_bytes < selective_bytes) {
        return RzfpReadStrategy::FullPayloadScan;
    }

    uint64_t whole_preads = 0;
    // Estimate whole-SB reads by reusing the same window merge over SB intervals.
    std::vector<ReadInterval> sb_intervals;
    sb_intervals.reserve(touched_sbs.size());
    for (uint64_t sbid : touched_sbs) {
        sb_intervals.push_back({sb_index[sbid].payload_offset, sb_index[sbid].payload_bytes, sbid});
    }
    std::sort(sb_intervals.begin(), sb_intervals.end(), [](const ReadInterval& a, const ReadInterval& b) {
        return a.offset < b.offset;
    });
    for (size_t i = 0; i < sb_intervals.size();) {
        uint64_t wstart = sb_intervals[i].offset;
        uint64_t wend = wstart + sb_intervals[i].size;
        size_t j = i + 1;
        while (j < sb_intervals.size()) {
            const uint64_t off = sb_intervals[j].offset;
            const uint64_t end = off + sb_intervals[j].size;
            if (off > wend + max_gap) break;
            if (end - wstart > read_window) break;
            wend = end;
            ++j;
        }
        ++whole_preads;
        i = j;
    }

    // Prefer whole-SB when it needs fewer reads and reads less total data.
    if (whole_sb_bytes < selective_bytes && whole_preads <= selective_preads / 2) {
        return RzfpReadStrategy::WholeSuperblock;
    }

    return RzfpReadStrategy::SelectiveLeaf;
}

static bool executeWindowedRead(
    int fd,
    const std::vector<ReadInterval>& intervals,
    const RzfpReaderConfig& config,
    DecodeCallback decode_cb,
    RzfpReadProfile& profile
) {
    if (intervals.empty()) return true;

    auto sorted = intervals;
    std::sort(sorted.begin(), sorted.end(), [](const ReadInterval& a, const ReadInterval& b) {
        return a.offset < b.offset;
    });

    const HDDReadWindowConfig& wcfg = config.hdd;
    const uint64_t read_window = wcfg.read_window_bytes > 0 ? wcfg.read_window_bytes : (16ULL * 1024 * 1024);
    const uint64_t max_gap = wcfg.max_gap_bytes;
    const int decode_threads = std::max(1, config.decode_threads);

    std::vector<std::unique_ptr<RzfpCodec>> codecs;
    for (int t = 0; t < decode_threads; ++t) {
        codecs.emplace_back(std::make_unique<RzfpCodec>());
    }
    ThreadPool pool(static_cast<size_t>(decode_threads), false);

    std::vector<uint8_t> window_buf;

    size_t i = 0;
    while (i < sorted.size()) {
        uint64_t wstart = sorted[i].offset;
        uint64_t wend = wstart + sorted[i].size;
        size_t j = i + 1;
        while (j < sorted.size()) {
            const uint64_t off = sorted[j].offset;
            const uint64_t end = off + sorted[j].size;
            if (off > wend + max_gap) break;
            if (end - wstart > read_window) break;
            wend = end;
            ++j;
        }

        const uint64_t wsize = wend - wstart;
        if (window_buf.size() < wsize) window_buf.resize(wsize);

        auto io_t0 = Clock::now();
        if (!readFullyAt(fd, window_buf.data(), wsize, wstart)) {
            std::cerr << "Error: RZFP read window failed at offset " << wstart << std::endl;
            return false;
        }
        profile.io_time_ms += msSince(io_t0);
        ++profile.pread_calls;
        profile.actual_read_bytes += wsize;

        auto dec_t0 = Clock::now();
        const size_t count = j - i;
        const int threads_to_use = static_cast<int>(std::min<size_t>(decode_threads, count));
        if (threads_to_use <= 1) {
            for (size_t k = i; k < j; ++k) {
                const auto& in = sorted[k];
                decode_cb(in.user, window_buf.data() + (in.offset - wstart), *codecs[0]);
            }
        } else {
            std::vector<std::future<void>> futures;
            const size_t per = (count + threads_to_use - 1) / threads_to_use;
            for (int t = 0; t < threads_to_use; ++t) {
                const size_t start = i + static_cast<size_t>(t) * per;
                const size_t end = std::min(start + per, j);
                if (start >= end) break;
                futures.push_back(pool.submit([&, start, end, t]() {
                    for (size_t k = start; k < end; ++k) {
                        const auto& in = sorted[k];
                        decode_cb(in.user, window_buf.data() + (in.offset - wstart), *codecs[t]);
                    }
                }));
            }
            for (auto& f : futures) f.get();
        }
        profile.decode_time_ms += msSince(dec_t0);

        i = j;
    }
    return true;
}

static bool executeSelectiveLeaf(
    int fd,
    const std::vector<RzfpLeafTask>& tasks,
    const ERWT3DHeader& plan_hdr,
    const RzfpReaderConfig& config,
    RzfpReadProfile& profile
) {
    std::vector<ReadInterval> intervals;
    intervals.reserve(tasks.size());
    for (size_t i = 0; i < tasks.size(); ++i) {
        intervals.push_back({tasks[i].file_offset, tasks[i].record_size, i});
        profile.requested_record_bytes += tasks[i].record_size;
    }

    auto decode_cb = [&](uint64_t user, const uint8_t* data, RzfpCodec& codec) {
        const auto& task = tasks[user];
        float leaf[64];
        if (!codec.decodeRecord(task.codec, data, task.record_size, leaf)) {
            std::cerr << "Error: RZFP decode failed for sb=" << task.physical_sb_id
                      << " morton=" << task.morton << std::endl;
            return;
        }
        for (const auto& sc : task.scatters) {
            scatterDecodedLeaf(plan_hdr, sc.op, leaf, sc.output);
        }
    };

    return executeWindowedRead(fd, intervals, config, decode_cb, profile);
}

static bool executeWholeSuperblock(
    int fd,
    const std::vector<RzfpLeafTask>& tasks,
    const std::unordered_map<uint64_t, std::vector<uint32_t>>& prefixes,
    const std::vector<RzfpLeafDescriptor>& descriptors,
    const std::vector<RzfpSuperblockIndex>& sb_index,
    const ERWT3DHeader& plan_hdr,
    const RzfpReaderConfig& config,
    RzfpReadProfile& profile
) {
    const uint64_t leavesPerSB = prefixes.begin()->second.size() - 1;
    auto groups = groupTasksBySuperblock(tasks);

    std::vector<ReadInterval> intervals;
    intervals.reserve(groups.size());
    for (const auto& kv : groups) {
        const uint64_t sbid = kv.first;
        intervals.push_back({sb_index[sbid].payload_offset, sb_index[sbid].payload_bytes, sbid});
        for (size_t ti : kv.second) {
            profile.requested_record_bytes += tasks[ti].record_size;
        }
    }

    auto decode_cb = [&](uint64_t sbid, const uint8_t* payload, RzfpCodec& codec) {
        const auto& prefix = prefixes.at(sbid);
        const uint64_t descBase = sbid * leavesPerSB;
        const auto it = groups.find(sbid);
        if (it == groups.end()) return;
        for (size_t ti : it->second) {
            const auto& task = tasks[ti];
            const uint8_t* record = payload + prefix[task.morton];
            float leaf[64];
            if (!codec.decodeRecord(task.codec, record, task.record_size, leaf)) {
                std::cerr << "Error: RZFP decode failed for sb=" << task.physical_sb_id
                          << " morton=" << task.morton << std::endl;
                return;
            }
            auto sc_t0 = Clock::now();
            for (const auto& sc : task.scatters) {
                scatterDecodedLeaf(plan_hdr, sc.op, leaf, sc.output);
            }
            }
    };

    return executeWindowedRead(fd, intervals, config, decode_cb, profile);
}

static bool executeFullPayloadScan(
    int fd,
    const std::vector<RzfpLeafTask>& tasks,
    const std::vector<RzfpSuperblockIndex>& sb_index,
    const std::vector<RzfpLeafDescriptor>& descriptors,
    const RzfpFileHeader& header,
    const ERWT3DHeader& plan_hdr,
    const RzfpReaderConfig& config,
    RzfpReadProfile& profile
) {
    const uint64_t leavesPerSB = rzfpTotalLeafsPerSuper(header);
    auto groups = groupTasksBySuperblock(tasks);

    std::vector<ReadInterval> intervals;
    intervals.reserve(sb_index.size());
    for (uint64_t sbid = 0; sbid < sb_index.size(); ++sbid) {
        intervals.push_back({sb_index[sbid].payload_offset, sb_index[sbid].payload_bytes, sbid});
    }
    for (const auto& t : tasks) profile.requested_record_bytes += t.record_size;

    auto decode_cb = [&](uint64_t sbid, const uint8_t* payload, RzfpCodec& codec) {
        auto it = groups.find(sbid);
        if (it == groups.end()) return;

        const uint64_t descBase = sbid * leavesPerSB;
        std::vector<uint32_t> prefix(leavesPerSB + 1, 0);
        for (uint64_t i = 0; i < leavesPerSB; ++i) {
            prefix[i + 1] = prefix[i] + descriptorSize(descriptors[descBase + i]);
        }

        for (size_t ti : it->second) {
            const auto& task = tasks[ti];
            const uint8_t* record = payload + prefix[task.morton];
            float leaf[64];
            if (!codec.decodeRecord(task.codec, record, task.record_size, leaf)) {
                std::cerr << "Error: RZFP decode failed for sb=" << task.physical_sb_id
                          << " morton=" << task.morton << std::endl;
                return;
            }
            auto sc_t0 = Clock::now();
            for (const auto& sc : task.scatters) {
                scatterDecodedLeaf(plan_hdr, sc.op, leaf, sc.output);
            }
            }
    };

    return executeWindowedRead(fd, intervals, config, decode_cb, profile);
}

} // namespace

RzfpReader::RzfpReader(const std::string& path) : path_(path), fd_(-1) {
    fd_ = open(path.c_str(), O_RDONLY);
    if (fd_ < 0) return;

    if (pread(fd_, &header_, sizeof(header_), 0) != sizeof(header_)) {
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

    sb_index_.resize(totalSB);
    const uint64_t indexBytes = totalSB * sizeof(RzfpSuperblockIndex);
    if (!readFullyAt(fd_, sb_index_.data(), indexBytes, sizeof(RzfpFileHeader))) {
        close(fd_);
        fd_ = -1;
        return;
    }

    descriptors_.resize(totalLeaves);
    const uint64_t descriptorBytes = totalLeaves * sizeof(RzfpLeafDescriptor);
    if (!readFullyAt(fd_, descriptors_.data(), descriptorBytes, header_.descriptor_offset)) {
        close(fd_);
        fd_ = -1;
        return;
    }
}

RzfpReader::~RzfpReader() {
    if (fd_ >= 0) close(fd_);
}

bool RzfpReader::readSlice(SliceAxis axis, uint64_t index, float* output,
                           int numThreads, size_t memoryLimitMB,
                           const HDDReadWindowConfig& wcfg) {
    SliceBatchRequest req{axis, index, output};
    return readSlicesBatch({req}, numThreads, memoryLimitMB, wcfg);
}

bool RzfpReader::readSlicesBatch(const std::vector<SliceBatchRequest>& requests,
                                 int numThreads, size_t memoryLimitMB,
                                 const HDDReadWindowConfig& wcfg) {
    (void)numThreads;
    (void)memoryLimitMB;
    RzfpReaderConfig config;
    config.hdd = wcfg;
    config.strategy = RzfpReadStrategy::Auto;
    config.decode_threads = 1;
    config.profile = nullptr;
    return readSlicesBatch(requests, config);
}

bool RzfpReader::readSlicesBatch(const std::vector<SliceBatchRequest>& requests,
                                 const RzfpReaderConfig& config) {
    if (fd_ < 0 || requests.empty()) return false;

    const auto t_start = Clock::now();
    const ERWT3DHeader plan_hdr = planHeaderFromRzfp(header_);
    const uint64_t leavesPerSB = rzfpTotalLeafsPerSuper(header_);

    double plan_time_ms = 0.0;
    auto tasks = buildLeafTasks(plan_hdr, requests, header_, plan_time_ms);
    if (tasks.empty()) return true;

    double prefix_time_ms = 0.0;
    computeTaskOffsets(tasks, descriptors_, sb_index_, leavesPerSB, prefix_time_ms);
    std::sort(tasks.begin(), tasks.end(), [](const RzfpLeafTask& a, const RzfpLeafTask& b) {
        return a.file_offset < b.file_offset;
    });

    RzfpReadProfile local_profile;
    RzfpReadProfile* profile = config.profile ? config.profile : &local_profile;
    profile->unique_leaves = tasks.size();
    std::unordered_set<uint64_t> unique_sbs;
    for (const auto& t : tasks) unique_sbs.insert(t.physical_sb_id);
    profile->unique_superblocks = unique_sbs.size();
    profile->plan_time_ms = plan_time_ms;
    profile->prefix_time_ms = prefix_time_ms;

    RzfpReadStrategy strategy = config.strategy;
    if (strategy == RzfpReadStrategy::Auto) {
        strategy = chooseStrategy(tasks, sb_index_, config, leavesPerSB);
    }

    auto prefixes = buildPrefixes(tasks, descriptors_, leavesPerSB);

    bool ok = false;
    switch (strategy) {
        case RzfpReadStrategy::SelectiveLeaf:
            ok = executeSelectiveLeaf(fd_, tasks, plan_hdr, config, *profile);
            break;
        case RzfpReadStrategy::WholeSuperblock:
            ok = executeWholeSuperblock(fd_, tasks, prefixes, descriptors_, sb_index_, plan_hdr, config, *profile);
            break;
        case RzfpReadStrategy::FullPayloadScan:
            ok = executeFullPayloadScan(fd_, tasks, sb_index_, descriptors_, header_, plan_hdr, config, *profile);
            break;
        default:
            ok = executeSelectiveLeaf(fd_, tasks, plan_hdr, config, *profile);
            break;
    }

    profile->plan_time_ms += msSince(t_start) - profile->io_time_ms - profile->decode_time_ms;
    if (profile->plan_time_ms < 0.0) profile->plan_time_ms = 0.0;

    return ok;
}

} // namespace erwt3d
