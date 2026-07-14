#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/morton.hpp"
#include "erwt3d/rzfp_codec.hpp"
#include "erwt3d/sb_plan.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace erwt3d {

namespace {

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
    const LeafOp* op = nullptr;
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

    if (fd_ < 0 || requests.empty()) return false;

    const ERWT3DHeader plan_hdr = planHeaderFromRzfp(header_);
    const uint64_t leavesPerSB = rzfpTotalLeafsPerSuper(header_);

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

    // Build unique leaf tasks.
    std::vector<RzfpLeafTask> tasks;
    tasks.reserve(1024);
    std::unordered_map<uint64_t, size_t> task_map;
    task_map.reserve(1024);

    for (size_t p = 0; p < plans.size(); ++p) {
        const auto& plan = plans[p];
        float* out = outputs[p];
        for (const auto& task : plan.tasks) {
            const uint64_t phys_sb = physicalSuperblockId(header_, task.sb_index);
            const LeafOp* ops = plan.leaf_ops.data() + task.first_leaf;
            for (uint32_t li = 0; li < task.leaf_count; ++li) {
                const LeafOp& op = ops[li];
                const uint64_t key = (phys_sb << 16) | op.morton;
                auto it = task_map.find(key);
                if (it == task_map.end()) {
                    RzfpLeafTask lt;
                    lt.physical_sb_id = phys_sb;
                    lt.morton = op.morton;
                    lt.scatters.push_back({&op, out});
                    const size_t idx = tasks.size();
                    tasks.push_back(std::move(lt));
                    task_map.emplace(key, idx);
                } else {
                    tasks[it->second].scatters.push_back({&op, out});
                }
            }
        }
    }

    if (tasks.empty()) return true;

    // Compute file offsets by prefix-summing descriptors per touched superblock.
    std::unordered_map<uint64_t, std::vector<uint32_t>> prefixes;
    prefixes.reserve(tasks.size());

    for (auto& t : tasks) {
        auto it = prefixes.find(t.physical_sb_id);
        if (it == prefixes.end()) {
            std::vector<uint32_t> prefix(leavesPerSB + 1, 0);
            const uint64_t descBase = t.physical_sb_id * leavesPerSB;
            for (uint64_t i = 0; i < leavesPerSB; ++i) {
                prefix[i + 1] = prefix[i] + descriptorSize(descriptors_[descBase + i]);
            }
            it = prefixes.emplace(t.physical_sb_id, std::move(prefix)).first;
        }
        const auto& prefix = it->second;
        const uint64_t descBase = t.physical_sb_id * leavesPerSB;
        const auto descriptor = descriptors_[descBase + t.morton];
        t.codec = descriptorCodec(descriptor);
        t.record_size = descriptorSize(descriptor);
        t.file_offset = sb_index_[t.physical_sb_id].payload_offset + prefix[t.morton];
    }

    std::sort(tasks.begin(), tasks.end(), [](const RzfpLeafTask& a, const RzfpLeafTask& b) {
        return a.file_offset < b.file_offset;
    });

    const uint64_t read_window = wcfg.read_window_bytes > 0 ? wcfg.read_window_bytes : (16ULL * 1024 * 1024);
    const uint64_t max_gap = wcfg.max_gap_bytes;

    RzfpCodec codec;
    std::vector<uint8_t> window_buf;
    float leaf[64];

    size_t ti = 0;
    uint64_t pread_calls = 0;
    uint64_t bytes_read = 0;

    while (ti < tasks.size()) {
        uint64_t wstart = tasks[ti].file_offset;
        uint64_t wend = wstart + tasks[ti].record_size;
        size_t tj = ti + 1;
        while (tj < tasks.size()) {
            const uint64_t off = tasks[tj].file_offset;
            const uint64_t end = off + tasks[tj].record_size;
            if (off > wend + max_gap) break;
            if (end - wstart > read_window) break;
            wend = end;
            ++tj;
        }

        const uint64_t wsize = wend - wstart;
        if (window_buf.size() < wsize) window_buf.resize(wsize);
        if (!readFullyAt(fd_, window_buf.data(), wsize, wstart)) {
            std::cerr << "Error: RZFP read window failed at offset " << wstart << std::endl;
            return false;
        }
        ++pread_calls;
        bytes_read += wsize;

        for (size_t k = ti; k < tj; ++k) {
            const auto& task = tasks[k];
            const uint8_t* record = window_buf.data() + (task.file_offset - wstart);

            RzfpCandidate cand;
            cand.codec = task.codec;
            cand.payload.assign(record, record + task.record_size);

            if (!codec.decode(cand, leaf)) {
                std::cerr << "Error: RZFP decode failed for sb=" << task.physical_sb_id
                          << " morton=" << task.morton << std::endl;
                return false;
            }

            for (const auto& sc : task.scatters) {
                scatterDecodedLeaf(plan_hdr, *sc.op, leaf, sc.output);
            }
        }

        ti = tj;
    }

    (void)pread_calls;
    (void)bytes_read;
    return true;
}

} // namespace erwt3d
