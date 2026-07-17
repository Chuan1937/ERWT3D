#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/morton.hpp"
#include "erwt3d/rzfp_codec.hpp"
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

using DecodeCallback = std::function<bool(uint64_t user, const uint8_t* data, RzfpCodec& codec)>;

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

static constexpr uint32_t PREFIX_CHECKPOINT_STRIDE = 16;

using PrefixCheckpointMap = std::unordered_map<uint64_t, std::vector<uint32_t>>;

static PrefixCheckpointMap buildPrefixCheckpoints(
    const std::vector<RzfpLeafTask>& tasks,
    const std::vector<RzfpLeafDescriptor>& descriptors,
    uint64_t leavesPerSB
) {
    std::unordered_map<uint64_t, std::vector<uint32_t>> checkpoints;
    checkpoints.reserve(tasks.size());
    std::unordered_set<uint64_t> seen;
    for (const auto& t : tasks) {
        if (!seen.insert(t.physical_sb_id).second) continue;
        uint64_t numCheckpoints = (leavesPerSB + PREFIX_CHECKPOINT_STRIDE - 1) / PREFIX_CHECKPOINT_STRIDE + 1;
        std::vector<uint32_t> cp(numCheckpoints, 0);
        const uint64_t descBase = t.physical_sb_id * leavesPerSB;
        uint32_t running = 0;
        for (uint64_t i = 0; i < leavesPerSB; ++i) {
            if (i % PREFIX_CHECKPOINT_STRIDE == 0) {
                cp[i / PREFIX_CHECKPOINT_STRIDE] = running;
            }
            running += descriptorSize(descriptors[descBase + i]);
        }
        cp[numCheckpoints - 1] = running;
        checkpoints.emplace(t.physical_sb_id, std::move(cp));
    }
    return checkpoints;
}

static uint32_t prefixFromCheckpoint(
    uint16_t morton,
    const std::vector<uint32_t>& checkpoints,
    const std::vector<RzfpLeafDescriptor>& descriptors,
    uint64_t descBase
) {
    uint32_t cpIdx = morton / PREFIX_CHECKPOINT_STRIDE;
    uint32_t base = checkpoints[cpIdx];
    uint64_t start = static_cast<uint64_t>(cpIdx) * PREFIX_CHECKPOINT_STRIDE;
    for (uint64_t i = start; i < morton; ++i) {
        base += descriptorSize(descriptors[descBase + i]);
    }
    return base;
}

static void computeTaskOffsets(
    std::vector<RzfpLeafTask>& tasks,
    const std::vector<RzfpLeafDescriptor>& descriptors,
    const std::vector<RzfpSuperblockIndex>& sb_index,
    uint64_t leavesPerSB,
    const PrefixCheckpointMap& checkpoints,
    double& prefix_time_ms
) {
    auto t0 = Clock::now();

    for (auto& t : tasks) {
        const uint64_t descBase = t.physical_sb_id * leavesPerSB;
        const auto descriptor = descriptors[descBase + t.morton];
        t.codec = descriptorCodec(descriptor);
        t.record_size = descriptorSize(descriptor);
        uint32_t prefix = prefixFromCheckpoint(t.morton, checkpoints.at(t.physical_sb_id),
                                                descriptors, descBase);
        t.file_offset = sb_index[t.physical_sb_id].payload_offset + prefix;
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
                                     : (512ULL * 1024 * 1024);
    const uint64_t max_gap = config.hdd.max_gap_bytes > 0
                                 ? config.hdd.max_gap_bytes
                                 : (8ULL * 1024 * 1024);

    // Drive parameters for the cost model.
    const double seek_ms = config.hdd.seek_ms > 0.0 ? config.hdd.seek_ms : 9.0;
    const double seq_mb_s = config.hdd.sequential_mb_s > 0.0 ? config.hdd.sequential_mb_s : 220.0;
    const double seq_byte_ms = 1.0 / (seq_mb_s * 1024.0 * 1024.0 / 1000.0);

    const auto estimateTime = [&](uint64_t bytes, uint64_t preads) -> double {
        return static_cast<double>(preads) * seek_ms + static_cast<double>(bytes) * seq_byte_ms;
    };

    uint64_t selective_preads = 0;
    const uint64_t selective_bytes = estimateSelectiveBytes(tasks, read_window, max_gap, selective_preads);

    uint64_t whole_sb_bytes = 0;
    for (uint64_t sbid : touched_sbs) whole_sb_bytes += sb_index[sbid].payload_bytes;

    const uint64_t fullscan_bytes = totalPayloadBytes(sb_index);
    const uint64_t totalLeaves = sb_index.size() * leavesPerSB;
    const double leaf_coverage = totalLeaves > 0
                                     ? static_cast<double>(tasks.size()) / static_cast<double>(totalLeaves)
                                     : 1.0;

    // Full payload scan reads everything in one large sequential sweep.
    // On HDD this is often faster than a selective read that drags in most
    // of the file through many small windows.
    const uint64_t fullscan_preads = std::max<uint64_t>(1, (fullscan_bytes + read_window - 1) / read_window);
    const double fullscan_time = estimateTime(fullscan_bytes, fullscan_preads);

    // Whole-superblock merges touched superblock payloads.
    uint64_t whole_preads = 0;
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

    const double selective_time = estimateTime(selective_bytes, selective_preads);
    const double whole_time = estimateTime(whole_sb_bytes, whole_preads);

    // If the selective read would already consume most of the payload, a single
    // sequential full scan wins due to lower seek overhead.
    if (selective_bytes > fullscan_bytes * 0.85 || fullscan_time < selective_time * 0.9) {
        if (leaf_coverage > 0.05 || fullscan_bytes < selective_bytes * 1.2) {
            return RzfpReadStrategy::FullPayloadScan;
        }
    }

    // Prefer whole-SB when it is clearly cheaper than selective.
    if (whole_time < selective_time && whole_sb_bytes < selective_bytes * 1.05) {
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
    const uint64_t read_window = wcfg.read_window_bytes > 0 ? wcfg.read_window_bytes : (512ULL * 1024 * 1024);
    const uint64_t max_gap = wcfg.max_gap_bytes > 0 ? wcfg.max_gap_bytes : (8ULL * 1024 * 1024);
    const int decode_threads = std::max(1, config.decode_threads);

    std::vector<std::unique_ptr<RzfpCodec>> codecs;
    for (int t = 0; t < decode_threads; ++t) {
        codecs.emplace_back(std::make_unique<RzfpCodec>());
    }
    ThreadPool pool(static_cast<size_t>(decode_threads), false);

    std::vector<uint8_t> window_buf;

    auto computeWindowEnd = [&](size_t start) -> std::pair<uint64_t, size_t> {
        uint64_t wstart = sorted[start].offset;
        uint64_t wend = wstart + sorted[start].size;
        size_t j = start + 1;
        while (j < sorted.size()) {
            const uint64_t off = sorted[j].offset;
            const uint64_t end = off + sorted[j].size;
            if (off > wend + max_gap) break;
            if (end - wstart > read_window) break;
            wend = end;
            ++j;
        }
        return {wend, j};
    };

    size_t i = 0;
    while (i < sorted.size()) {
        const uint64_t wstart = sorted[i].offset;
        auto [wend, j] = computeWindowEnd(i);

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

        // Prefetch the next window so the drive can overlap seek/transfer
        // with the decode phase of the current window.
        if (j < sorted.size()) {
            const uint64_t next_start = sorted[j].offset;
            auto [next_end, next_j] = computeWindowEnd(j);
            (void)next_j;
            const uint64_t next_size = next_end - next_start;
            readahead(fd, static_cast<off_t>(next_start), static_cast<size_t>(next_size));
        }

        auto dec_t0 = Clock::now();
        const size_t count = j - i;
        const int threads_to_use = static_cast<int>(std::min<size_t>(decode_threads, count));
        bool decode_ok = true;
        if (threads_to_use <= 1) {
            for (size_t k = i; k < j; ++k) {
                const auto& in = sorted[k];
                if (!decode_cb(in.user, window_buf.data() + (in.offset - wstart), *codecs[0])) {
                    decode_ok = false;
                }
            }
        } else {
            std::vector<std::future<bool>> futures;
            const size_t per = (count + threads_to_use - 1) / threads_to_use;
            for (int t = 0; t < threads_to_use; ++t) {
                const size_t start = i + static_cast<size_t>(t) * per;
                const size_t end = std::min(start + per, j);
                if (start >= end) break;
                futures.push_back(pool.submit([&, start, end, t]() {
                    bool ok = true;
                    for (size_t k = start; k < end; ++k) {
                        const auto& in = sorted[k];
                        if (!decode_cb(in.user, window_buf.data() + (in.offset - wstart), *codecs[t])) {
                            ok = false;
                        }
                    }
                    return ok;
                }));
            }
            for (auto& f : futures) {
                if (!f.get()) decode_ok = false;
            }
        }
        profile.decode_time_ms += msSince(dec_t0);

        if (!decode_ok) {
            std::cerr << "Error: RZFP decode failed in window starting at offset " << wstart << std::endl;
            return false;
        }

        i = j;
    }
    profile.scatter_time_ms = static_cast<double>(profile.scatter_ns.load()) * 1e-6;
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

    auto decode_cb = [&](uint64_t user, const uint8_t* data, RzfpCodec& codec) -> bool {
        const auto& task = tasks[user];
        float leaf[64];
        if (!codec.decodeRecord(task.codec, data, task.record_size, leaf)) {
            std::cerr << "Error: RZFP decode failed for sb=" << task.physical_sb_id
                      << " morton=" << task.morton << std::endl;
            return false;
        }
        auto sc_t0 = Clock::now();
        for (const auto& sc : task.scatters) {
            scatterDecodedLeaf(plan_hdr, sc.op, leaf, sc.output);
        }
        profile.scatter_ns.fetch_add(nsSince(sc_t0), std::memory_order_relaxed);
        return true;
    };

    return executeWindowedRead(fd, intervals, config, decode_cb, profile);
}

static bool executeWholeSuperblock(
    int fd,
    const std::vector<RzfpLeafTask>& tasks,
    const PrefixCheckpointMap& checkpoints,
    const std::vector<RzfpLeafDescriptor>& descriptors,
    const std::vector<RzfpSuperblockIndex>& sb_index,
    const ERWT3DHeader& plan_hdr,
    const RzfpReaderConfig& config,
    RzfpReadProfile& profile,
    uint64_t leavesPerSB
) {
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

    auto decode_cb = [&](uint64_t sbid, const uint8_t* payload, RzfpCodec& codec) -> bool {
        const auto it = groups.find(sbid);
        if (it == groups.end()) return true;
        const auto& cp = checkpoints.at(sbid);
        const uint64_t descBase = sbid * leavesPerSB;
        bool ok = true;
        for (size_t ti : it->second) {
            const auto& task = tasks[ti];
            uint32_t off = prefixFromCheckpoint(task.morton, cp, descriptors, descBase);
            const uint8_t* record = payload + off;
            float leaf[64];
            if (!codec.decodeRecord(task.codec, record, task.record_size, leaf)) {
                std::cerr << "Error: RZFP decode failed for sb=" << task.physical_sb_id
                          << " morton=" << task.morton << std::endl;
                ok = false;
                continue;
            }
            auto sc_t0 = Clock::now();
            for (const auto& sc : task.scatters) {
                scatterDecodedLeaf(plan_hdr, sc.op, leaf, sc.output);
            }
            profile.scatter_ns.fetch_add(nsSince(sc_t0), std::memory_order_relaxed);
        }
        return ok;
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
    RzfpReadProfile& profile,
    const PrefixCheckpointMap& checkpoints
) {
    const uint64_t leavesPerSB = rzfpTotalLeafsPerSuper(header);
    auto groups = groupTasksBySuperblock(tasks);

    std::vector<ReadInterval> intervals;
    intervals.reserve(sb_index.size());
    for (uint64_t sbid = 0; sbid < sb_index.size(); ++sbid) {
        intervals.push_back({sb_index[sbid].payload_offset, sb_index[sbid].payload_bytes, sbid});
    }
    for (const auto& t : tasks) profile.requested_record_bytes += t.record_size;

    auto decode_cb = [&](uint64_t sbid, const uint8_t* payload, RzfpCodec& codec) -> bool {
        auto it = groups.find(sbid);
        if (it == groups.end()) return true;

        const uint64_t descBase = sbid * leavesPerSB;
        const auto& cp = checkpoints.at(sbid);

        bool ok = true;
        for (size_t ti : it->second) {
            const auto& task = tasks[ti];
            uint32_t off = prefixFromCheckpoint(task.morton, cp, descriptors, descBase);
            const uint8_t* record = payload + off;
            float leaf[64];
            if (!codec.decodeRecord(task.codec, record, task.record_size, leaf)) {
                std::cerr << "Error: RZFP decode failed for sb=" << task.physical_sb_id
                          << " morton=" << task.morton << std::endl;
                ok = false;
                continue;
            }
            auto sc_t0 = Clock::now();
            for (const auto& sc : task.scatters) {
                scatterDecodedLeaf(plan_hdr, sc.op, leaf, sc.output);
            }
            profile.scatter_ns.fetch_add(nsSince(sc_t0), std::memory_order_relaxed);
        }
        return ok;
    };

    return executeWindowedRead(fd, intervals, config, decode_cb, profile);
}

#pragma pack(push, 1)
struct XPlaneHeader {
    char magic[8];
    uint64_t version;
    uint64_t nx, ny, nz;
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

RzfpReader::RzfpReader(const std::string& path) : path_(path), fd_(-1) {
    fd_ = open(path.c_str(), O_RDONLY);
    if (fd_ < 0) return;

    // Get file size before any allocation
    struct stat st;
    if (fstat(fd_, &st) != 0) {
        close(fd_);
        fd_ = -1;
        return;
    }
    const uint64_t fileSize = static_cast<uint64_t>(st.st_size);

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
    const uint64_t indexBytes = totalSB * sizeof(RzfpSuperblockIndex);
    const uint64_t descriptorBytes = totalLeaves * sizeof(RzfpLeafDescriptor);

    // Validate region ordering and bounds before allocation
    if (sizeof(RzfpFileHeader) > fileSize ||
        indexBytes > fileSize - sizeof(RzfpFileHeader)) {
        close(fd_); fd_ = -1; return;
    }
    if (header_.descriptor_offset < sizeof(RzfpFileHeader) + indexBytes ||
        descriptorBytes > fileSize - header_.descriptor_offset) {
        close(fd_); fd_ = -1; return;
    }
    if (header_.payload_offset < header_.descriptor_offset + descriptorBytes ||
        header_.payload_offset > fileSize) {
        close(fd_); fd_ = -1; return;
    }

    sb_index_.resize(totalSB);
    if (!readFullyAt(fd_, sb_index_.data(), indexBytes, sizeof(RzfpFileHeader))) {
        close(fd_);
        fd_ = -1;
        return;
    }

    descriptors_.resize(totalLeaves);
    if (!readFullyAt(fd_, descriptors_.data(), descriptorBytes, header_.descriptor_offset)) {
        close(fd_);
        fd_ = -1;
        return;
    }

    // Validate superblock index entries
    for (uint64_t i = 0; i < totalSB; ++i) {
        const auto& idx = sb_index_[i];
        if (idx.payload_offset < header_.payload_offset ||
            idx.payload_offset > fileSize ||
            idx.payload_bytes > fileSize - idx.payload_offset) {
            close(fd_); fd_ = -1; return;
        }
    }

    // Validate descriptors: codec range and per-SB size sum
    for (uint64_t i = 0; i < totalLeaves; ++i) {
        if (static_cast<uint8_t>(descriptorCodec(descriptors_[i])) > 5) {
            close(fd_); fd_ = -1; return;
        }
    }
    for (uint64_t sb = 0; sb < totalSB; ++sb) {
        uint64_t leafStart = sb * rzfpTotalLeafsPerSuper(header_);
        uint64_t sumSizes = 0;
        for (uint64_t j = 0; j < rzfpTotalLeafsPerSuper(header_); ++j) {
            uint64_t li = leafStart + j;
            if (li >= totalLeaves) break;
            sumSizes += descriptorSize(descriptors_[li]);
        }
        if (sumSizes != sb_index_[sb].payload_bytes) {
            close(fd_); fd_ = -1; return;
        }
    }

    openXPlaneSidecar();
    initRawXAux_();
}

RzfpReader::~RzfpReader() {
    if (rawXAuxFd_ >= 0) close(rawXAuxFd_);
    if (fd_ >= 0) close(fd_);
    if (xplane_fd_ >= 0) close(xplane_fd_);
}

void RzfpReader::initRawXAux_() {
    if (!hasRawXAux(header_)) return;

    RawXAuxRegion region;
    region.offset = rzfpRawXAuxOffset(header_);
    region.bytes = rzfpRawXAuxBytes(header_);
    region.plane_bytes = rzfpRawXAuxPlaneBytes(header_);
    region.version = rzfpRawXAuxVersion(header_);

    struct stat st;
    if (fstat(fd_, &st) != 0) return;
    uint64_t fileSize = static_cast<uint64_t>(st.st_size);

    // Minimum offset: after all RZFP payload
    uint64_t minimumOffset = header_.payload_offset;
    for (const auto& sb : sb_index_) {
        uint64_t end = 0;
        if (checkedAddU64(sb.payload_offset, sb.payload_bytes, end)) {
            if (end > minimumOffset) minimumOffset = end;
        }
    }

    auto err = validateRawXAuxRegion(fileSize, minimumOffset,
                                      header_.nx, header_.ny, header_.nz, region);
    if (err != RawXAuxValidationError::None) {
        std::cerr << "Warning: RZFP Raw X auxiliary validation failed: "
                  << rawXAuxValidationErrorStr(err)
                  << " — falling back to main file reader" << std::endl;
        return;
    }

    rawXAuxOffset_ = region.offset;
    rawXAuxPlaneBytes_ = region.plane_bytes;

    rawXAuxFd_ = open(path_.c_str(), O_RDONLY);
    if (rawXAuxFd_ < 0) return;

    rawXAuxAvailable_ = true;
}

bool RzfpReader::tryReadSliceRawXAux_(uint64_t x, float* output, RzfpReadProfile* profile) {
    if (!rawXAuxAvailable_ || x >= header_.nx) return false;

    uint64_t planeBytes = header_.ny * header_.nz * sizeof(float);
    uint64_t offset = rawXAuxOffset_ + x * rawXAuxPlaneBytes_;

    auto io_t0 = Clock::now();
    ssize_t n = pread(rawXAuxFd_, output, planeBytes, offset);
    if (n != static_cast<ssize_t>(planeBytes)) return false;

    if (profile) {
        profile->io_time_ms += msSince(io_t0);
        ++profile->pread_calls;
        profile->actual_read_bytes += planeBytes;
        profile->requested_record_bytes += planeBytes;
    }

    posix_fadvise(rawXAuxFd_, static_cast<off_t>(offset),
                  static_cast<off_t>(planeBytes), POSIX_FADV_DONTNEED);

    return true;
}

bool RzfpReader::tryReadBatchRawXAux_(
    const std::vector<SliceBatchRequest>& requests,
    const RzfpReaderConfig& config,
    RzfpReadProfile& profile) {

    if (!rawXAuxAvailable_ || requests.empty()) return false;

    struct RawXAuxTask {
        uint64_t x;
        uint64_t file_offset;
        float* output;
    };

    std::vector<RawXAuxTask> tasks;
    tasks.reserve(requests.size());
    bool anyHit = false;

    for (const auto& req : requests) {
        if (req.axis != SliceAxis::X) continue;
        uint64_t x = req.index;
        if (x >= header_.nx) continue;
        anyHit = true;
        tasks.push_back({x, rawXAuxOffset_ + x * rawXAuxPlaneBytes_, req.output});
        profile.requested_record_bytes += header_.ny * header_.nz * sizeof(float);
    }

    if (!anyHit) return false;
    if (tasks.empty()) return true;

    uint64_t planeBytes = header_.ny * header_.nz * sizeof(float);

    std::sort(tasks.begin(), tasks.end(),
              [](const RawXAuxTask& a, const RawXAuxTask& b) {
                  return a.file_offset < b.file_offset;
              });

    const uint64_t MAX_WINDOW = 256ULL * 1024 * 1024;

    size_t i = 0;
    while (i < tasks.size()) {
        uint64_t wstart = tasks[i].file_offset;
        uint64_t wend = wstart + planeBytes;
        size_t j = i + 1;

        while (j < tasks.size()) {
            uint64_t next_offset = tasks[j].file_offset;
            if (next_offset > wend + planeBytes) break;
            uint64_t proposed_end = next_offset + planeBytes;
            if (proposed_end - wstart > MAX_WINDOW) break;
            wend = proposed_end;
            ++j;
        }

        uint64_t wsize = wend - wstart;

        std::vector<uint8_t> winBuf(wsize);
        auto io_t0 = Clock::now();
        ssize_t n = pread(rawXAuxFd_, winBuf.data(), wsize, wstart);
        if (n != static_cast<ssize_t>(wsize)) return false;

        profile.io_time_ms += msSince(io_t0);
        ++profile.pread_calls;
        profile.actual_read_bytes += wsize;

        for (size_t k = i; k < j; ++k) {
            uint64_t off_in_window = tasks[k].file_offset - wstart;
            std::memcpy(tasks[k].output, winBuf.data() + off_in_window, planeBytes);
        }

        posix_fadvise(rawXAuxFd_, static_cast<off_t>(wstart),
                      static_cast<off_t>(wsize), POSIX_FADV_DONTNEED);

        i = j;
    }

    return true;
}

bool RzfpReader::openXPlaneSidecar() {
    const std::string sidecar_path = path_ + ".xp";
    int fd = open(sidecar_path.c_str(), O_RDONLY);
    if (fd < 0) return false;

    XPlaneHeader hdr{};
    if (pread(fd, &hdr, sizeof(hdr), 0) != sizeof(hdr)) {
        close(fd);
        return false;
    }

    const char expected_magic[8] = {'E', 'R', 'W', 'T', '3', 'D', 'X', ' '};
    if (!magicMatches(hdr.magic, expected_magic) || hdr.version != 1) {
        close(fd);
        return false;
    }

    if (hdr.nx != header_.nx || hdr.ny != header_.ny || hdr.nz != header_.nz) {
        close(fd);
        return false;
    }

    xplane_offsets_.resize(hdr.nx);
    xplane_sizes_.resize(hdr.nx);
    std::vector<XPlaneIndexEntry> entries(hdr.nx);
    const uint64_t index_bytes = hdr.nx * sizeof(XPlaneIndexEntry);
    if (!readFullyAt(fd, entries.data(), index_bytes, sizeof(XPlaneHeader))) {
        close(fd);
        return false;
    }

    // Validate entry bounds
    struct stat xpStat;
    uint64_t xpFileSize = 0;
    if (fstat(fd, &xpStat) == 0) {
        xpFileSize = static_cast<uint64_t>(xpStat.st_size);
    }
    for (uint64_t i = 0; i < hdr.nx; ++i) {
        const auto& e = entries[i];
        if (xpFileSize > 0) {
            if (e.size == 0 || e.size > 512 * 1024 * 1024) {  // max 512 MB per plane
                close(fd); return false;
            }
            if (e.offset > xpFileSize || e.size > xpFileSize - e.offset) {
                close(fd); return false;
            }
        }
        xplane_offsets_[i] = e.offset;
        xplane_sizes_[i] = e.size;
    }

    xplane_fd_ = fd;
    has_xplane_ = true;
    return true;
}

bool RzfpReader::readXPlaneFromSidecar(uint64_t x, float* output, RzfpReadProfile* profile) {
    if (!has_xplane_ || x >= xplane_offsets_.size()) return false;

    const uint64_t offset = xplane_offsets_[x];
    const uint32_t size = xplane_sizes_[x];

    auto io_t0 = Clock::now();
    std::vector<uint8_t> record(size);
    if (!readFullyAt(xplane_fd_, record.data(), size, offset)) return false;
    if (profile) {
        profile->io_time_ms += msSince(io_t0);
        ++profile->pread_calls;
        profile->actual_read_bytes += size;
        profile->requested_record_bytes += size;
    }

    auto dec_t0 = Clock::now();
    std::vector<float> plane(header_.ny * header_.nz);
    bool ok = decodeXPlane2D(record.data(), size, plane.data(), header_.ny, header_.nz);
    if (profile) profile->decode_time_ms += msSince(dec_t0);
    if (!ok) return false;

    // Sidecar stores X-plane in Y-fastest layout (z*ny + y).
    // Reader X-slice output follows official Z-fastest layout (y*nz + z).
    for (uint64_t y = 0; y < header_.ny; ++y) {
        for (uint64_t z = 0; z < header_.nz; ++z) {
            output[y * header_.nz + z] = plane[z * header_.ny + y];
        }
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
        uint64_t x = 0;
        uint64_t offset = 0;
        uint32_t size = 0;
        float* output = nullptr;
    };

    std::vector<XTask> tasks;
    tasks.reserve(requests.size());
    for (const auto& req : requests) {
        if (req.index >= xplane_offsets_.size()) return false;
        XTask t;
        t.x = req.index;
        t.offset = xplane_offsets_[req.index];
        t.size = xplane_sizes_[req.index];
        t.output = req.output;
        tasks.push_back(t);
        profile.requested_record_bytes += t.size;
    }

    std::sort(tasks.begin(), tasks.end(), [](const XTask& a, const XTask& b) {
        return a.offset < b.offset;
    });

    const HDDReadWindowConfig& wcfg = config.hdd;
    const uint64_t read_window = wcfg.read_window_bytes > 0 ? wcfg.read_window_bytes : (512ULL * 1024 * 1024);
    const uint64_t max_gap = wcfg.max_gap_bytes > 0 ? wcfg.max_gap_bytes : (8ULL * 1024 * 1024);
    const int decode_threads = std::max(1, config.decode_threads);

    ThreadPool pool(static_cast<size_t>(decode_threads), false);
    std::vector<std::unique_ptr<RzfpCodec>> codecs;
    for (int t = 0; t < decode_threads; ++t) {
        codecs.emplace_back(std::make_unique<RzfpCodec>());
    }

    std::vector<uint8_t> window_buf;

    size_t i = 0;
    while (i < tasks.size()) {
        uint64_t wstart = tasks[i].offset;
        uint64_t wend = wstart + tasks[i].size;
        size_t j = i + 1;
        while (j < tasks.size()) {
            const uint64_t off = tasks[j].offset;
            const uint64_t end = off + tasks[j].size;
            if (off > wend + max_gap) break;
            if (end - wstart > read_window) break;
            wend = end;
            ++j;
        }

        const uint64_t wsize = wend - wstart;
        if (window_buf.size() < wsize) window_buf.resize(wsize);

        auto io_t0 = Clock::now();
        if (!readFullyAt(xplane_fd_, window_buf.data(), wsize, wstart)) {
            std::cerr << "Error: RZFP sidecar batch read failed at offset " << wstart << std::endl;
            return false;
        }
        profile.io_time_ms += msSince(io_t0);
        ++profile.pread_calls;
        profile.actual_read_bytes += wsize;

        auto dec_t0 = Clock::now();
        const size_t count = j - i;
        const int threads_to_use = static_cast<int>(std::min<size_t>(decode_threads, count));
        bool decode_ok = true;
        std::vector<std::vector<float>> planes(count, std::vector<float>(header_.ny * header_.nz));
        if (threads_to_use <= 1) {
            for (size_t k = i; k < j; ++k) {
                const auto& task = tasks[k];
                if (!decodeXPlane2D(window_buf.data() + (task.offset - wstart), task.size,
                                    planes[k - i].data(), header_.ny, header_.nz)) {
                    decode_ok = false;
                    break;
                }
            }
        } else {
            std::vector<std::future<bool>> futures;
            const size_t per = (count + threads_to_use - 1) / threads_to_use;
            for (int t = 0; t < threads_to_use; ++t) {
                const size_t start = i + static_cast<size_t>(t) * per;
                const size_t end = std::min(start + per, j);
                if (start >= end) break;
                futures.push_back(pool.submit([&, start, end, t]() {
                    bool ok = true;
                    for (size_t k = start; k < end && ok; ++k) {
                        const auto& task = tasks[k];
                        ok = decodeXPlane2D(window_buf.data() + (task.offset - wstart), task.size,
                                            planes[k - i].data(), header_.ny, header_.nz);
                    }
                    return ok;
                }));
            }
            for (auto& f : futures) {
                if (!f.get()) decode_ok = false;
            }
        }

        // Transpose each decoded Y-fastest plane to official Z-fastest output.
        for (size_t k = i; k < j; ++k) {
            const auto& task = tasks[k];
            const auto& plane = planes[k - i];
            for (uint64_t y = 0; y < header_.ny; ++y) {
                for (uint64_t z = 0; z < header_.nz; ++z) {
                    task.output[y * header_.nz + z] = plane[z * header_.ny + y];
                }
            }
        }
        profile.decode_time_ms += msSince(dec_t0);

        if (!decode_ok) {
            std::cerr << "Error: RZFP sidecar batch decode failed" << std::endl;
            return false;
        }

        i = j;
    }

    return true;
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
    RzfpReaderConfig config;
    config.hdd = wcfg;
    config.strategy = RzfpReadStrategy::Auto;
    config.decode_threads = std::max(1, numThreads);
    config.profile = nullptr;

    // Apply a memory-aware read window if the caller constrained the budget.
    // Keep at least one window buffer plus headroom for output buffers.
    if (memoryLimitMB > 0 && config.hdd.read_window_bytes == 0) {
        const uint64_t auto_window = 512ULL * 1024 * 1024;
        const uint64_t budget_window = static_cast<uint64_t>(memoryLimitMB) * 1024 * 1024 / 4;
        config.hdd.read_window_bytes = std::min(auto_window, std::max<uint64_t>(16ULL * 1024 * 1024, budget_window));
    }
    if (memoryLimitMB > 0 && config.hdd.max_gap_bytes == 0) {
        config.hdd.max_gap_bytes = std::max<uint64_t>(1ULL * 1024 * 1024,
                                                      config.hdd.read_window_bytes / 64);
    }

    return readSlicesBatch(requests, config);
}

bool RzfpReader::readSlicesBatch(const std::vector<SliceBatchRequest>& requests,
                                 const RzfpReaderConfig& config) {
    if (fd_ < 0 || requests.empty()) return false;

    RzfpReadProfile local_profile;
    RzfpReadProfile* profile = config.profile ? config.profile : &local_profile;

    std::vector<SliceBatchRequest> fallback;
    fallback.reserve(requests.size());

    const auto t_plan = Clock::now();

    // Try raw X auxiliary first (fastest path for X requests)
    if (rawXAuxAvailable_) {
        SliceBatchRequest xSel;
        xSel.axis = SliceAxis::X;
        std::vector<SliceBatchRequest> x_requests;
        x_requests.reserve(requests.size());
        for (const auto& req : requests) {
            if (req.axis == SliceAxis::X) {
                x_requests.push_back(req);
            } else {
                fallback.push_back(req);
            }
        }
        if (!x_requests.empty()) {
            if (!tryReadBatchRawXAux_(x_requests, config, *profile)) {
                fallback.insert(fallback.end(), x_requests.begin(), x_requests.end());
            }
        }
    } else {
        std::vector<SliceBatchRequest> x_requests;
        x_requests.reserve(requests.size());
        for (const auto& req : requests) {
            if (has_xplane_ && req.axis == SliceAxis::X) {
                x_requests.push_back(req);
            } else {
                fallback.push_back(req);
            }
        }
        if (!x_requests.empty()) {
            if (!readXPlanesBatchFromSidecar(x_requests, config, *profile)) {
                fallback.insert(fallback.end(), x_requests.begin(), x_requests.end());
            }
        }
    }

    // Plan time: only the request classification above (not sidecar I/O)
    profile->plan_time_ms += msSince(t_plan);

    if (fallback.empty()) {
        return true;
    }

    const ERWT3DHeader plan_hdr = planHeaderFromRzfp(header_);
    const uint64_t leavesPerSB = rzfpTotalLeafsPerSuper(header_);

    double plan_time_ms = 0.0;
    auto tasks = buildLeafTasks(plan_hdr, fallback, header_, plan_time_ms);
    if (tasks.empty()) {
        profile->plan_time_ms += plan_time_ms;
        return true;
    }

    double prefix_time_ms = 0.0;
    auto checkpoints = buildPrefixCheckpoints(tasks, descriptors_, leavesPerSB);
    computeTaskOffsets(tasks, descriptors_, sb_index_, leavesPerSB, checkpoints, prefix_time_ms);
    std::sort(tasks.begin(), tasks.end(), [](const RzfpLeafTask& a, const RzfpLeafTask& b) {
        return a.file_offset < b.file_offset;
    });

    profile->unique_leaves += tasks.size();
    std::unordered_set<uint64_t> unique_sbs;
    for (const auto& t : tasks) unique_sbs.insert(t.physical_sb_id);
    profile->unique_superblocks += unique_sbs.size();
    profile->plan_time_ms += plan_time_ms;
    profile->prefix_time_ms += prefix_time_ms;

    RzfpReadStrategy strategy = config.strategy;
    if (strategy == RzfpReadStrategy::Auto) {
        strategy = chooseStrategy(tasks, sb_index_, config, leavesPerSB);
    }
    profile->selected_strategy = strategy;

    bool ok = false;
    switch (strategy) {
        case RzfpReadStrategy::SelectiveLeaf:
            ok = executeSelectiveLeaf(fd_, tasks, plan_hdr, config, *profile);
            break;
        case RzfpReadStrategy::WholeSuperblock:
            ok = executeWholeSuperblock(fd_, tasks, checkpoints, descriptors_, sb_index_, plan_hdr, config, *profile, leavesPerSB);
            break;
        case RzfpReadStrategy::FullPayloadScan:
            ok = executeFullPayloadScan(fd_, tasks, sb_index_, descriptors_, header_, plan_hdr, config, *profile, checkpoints);
            break;
        default:
            ok = executeSelectiveLeaf(fd_, tasks, plan_hdr, config, *profile);
            break;
    }

    return ok;
}

} // namespace erwt3d
