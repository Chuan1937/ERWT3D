#include "erwt3d/rzfp_round_plan.hpp"
#include "erwt3d/morton.hpp"
#include "erwt3d/sb_plan.hpp"
#include "erwt3d/rzfp_format.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace erwt3d {

namespace {

constexpr uint32_t PREFIX_CHECKPOINT_STRIDE = 16;

struct InternalLeafTask {
    uint64_t physical_sb_id = 0;
    uint16_t morton = 0;
    RoundScatterTarget target;
};

uint64_t computePhysicalSuperblockId(
    const RzfpFileHeader& header,
    uint64_t logical_id
) {
    const uint64_t sgX = rzfpSuperGridX(header);
    const uint64_t sgY = rzfpSuperGridY(header);
    const uint64_t sx = logical_id % sgX;
    const uint64_t rem = logical_id / sgX;
    const uint64_t sy = rem % sgY;
    const uint64_t sz = rem / sgY;
    return rzfpSuperblockId(
        header,
        sz,
        sy,
        sx,
        (header.flags & FLAG_PHYSICAL_ORDER_YZX)
            ? PhysicalOrder::V05_YZX
            : PhysicalOrder::ZYX
    );
}

std::unordered_map<uint64_t, std::vector<uint32_t>> buildPrefixCheckpoints(
    const std::unordered_set<uint64_t>& unique_sb_ids,
    const std::vector<RzfpLeafDescriptor>& descriptors,
    uint64_t leaves_per_sb
) {
    std::unordered_map<uint64_t, std::vector<uint32_t>> checkpoints;
    checkpoints.reserve(unique_sb_ids.size());

    for (uint64_t sb_id : unique_sb_ids) {
        const uint64_t count =
            (leaves_per_sb + PREFIX_CHECKPOINT_STRIDE - 1) /
                PREFIX_CHECKPOINT_STRIDE + 1;
        std::vector<uint32_t> cp(count, 0);
        const uint64_t descriptor_base = sb_id * leaves_per_sb;
        uint32_t running = 0;

        for (uint64_t i = 0; i < leaves_per_sb; ++i) {
            if (i % PREFIX_CHECKPOINT_STRIDE == 0) {
                cp[i / PREFIX_CHECKPOINT_STRIDE] = running;
            }
            running += descriptorSize(descriptors[descriptor_base + i]);
        }
        cp[count - 1] = running;
        checkpoints.emplace(sb_id, std::move(cp));
    }

    return checkpoints;
}

uint32_t prefixFromCheckpoint(
    uint16_t morton,
    const std::vector<uint32_t>& checkpoints,
    const std::vector<RzfpLeafDescriptor>& descriptors,
    uint64_t descriptor_base
) {
    const uint32_t checkpoint_index = morton / PREFIX_CHECKPOINT_STRIDE;
    uint32_t base = checkpoints[checkpoint_index];
    const uint64_t start =
        static_cast<uint64_t>(checkpoint_index) * PREFIX_CHECKPOINT_STRIDE;
    for (uint64_t i = start; i < morton; ++i) {
        base += descriptorSize(descriptors[descriptor_base + i]);
    }
    return base;
}

void computeTaskOffsets(
    std::vector<RoundLeafTask>& tasks,
    const std::vector<RzfpLeafDescriptor>& descriptors,
    const std::vector<RzfpSuperblockIndex>& sb_index,
    uint64_t leaves_per_sb,
    const std::unordered_map<uint64_t, std::vector<uint32_t>>& checkpoints
) {
    for (auto& task : tasks) {
        const auto it = checkpoints.find(task.physical_superblock_id);
        if (it == checkpoints.end()) continue;

        const uint64_t descriptor_base =
            task.physical_superblock_id * leaves_per_sb;
        const uint32_t prefix = prefixFromCheckpoint(
            task.morton,
            it->second,
            descriptors,
            descriptor_base
        );

        const auto& sbi = sb_index[task.physical_superblock_id];
        task.file_offset = sbi.payload_offset + prefix;
        task.record_size =
            descriptorSize(descriptors[descriptor_base + task.morton]);
        task.codec =
            descriptorCodec(descriptors[descriptor_base + task.morton]);
    }
}

ERWT3DHeader planHeaderFromRzfp(const RzfpFileHeader& rh) {
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

std::vector<RoundReadInterval> buildIntervals(
    std::vector<RoundLeafTask>& tasks,
    const HDDReadWindowConfig& window_config
) {
    std::vector<RoundReadInterval> intervals;
    if (tasks.empty()) return intervals;

    std::sort(
        tasks.begin(),
        tasks.end(),
        [](const RoundLeafTask& a, const RoundLeafTask& b) {
            return a.file_offset < b.file_offset;
        }
    );

    const uint64_t window_bytes = window_config.read_window_bytes > 0
        ? window_config.read_window_bytes
        : 512ULL * 1024 * 1024;
    const uint64_t max_gap = window_config.max_gap_bytes > 0
        ? window_config.max_gap_bytes
        : 8ULL * 1024 * 1024;

    size_t i = 0;
    while (i < tasks.size()) {
        RoundReadInterval interval;
        interval.first_task = i;
        interval.offset = tasks[i].file_offset;
        uint64_t window_end = interval.offset;
        uint64_t last_record_end = interval.offset +
            static_cast<uint64_t>(tasks[i].record_size);

        size_t j = i + 1;
        while (j < tasks.size()) {
            const uint64_t next_start = tasks[j].file_offset;
            const uint64_t next_end = next_start +
                static_cast<uint64_t>(tasks[j].record_size);

            if (next_start > window_end + max_gap) break;
            if (next_end - interval.offset > window_bytes) break;

            last_record_end = std::max(last_record_end, next_end);
            window_end = std::max(window_end, next_start);
            ++j;
        }

        interval.task_count = j - i;
        interval.size = last_record_end - interval.offset;
        intervals.push_back(interval);
        i = j;
    }

    return intervals;
}

} // anonymous namespace

RzfpRoundPlan buildRzfpRoundPlan(
    const RzfpFileHeader& header,
    const std::vector<RzfpSuperblockIndex>& superblock_index,
    const std::vector<RzfpLeafDescriptor>& descriptors,
    const std::vector<RoundSliceRequest>& requests,
    const HDDReadWindowConfig& window_config
) {
    RzfpRoundPlan plan;
    if (requests.empty()) return plan;

    const ERWT3DHeader plan_hdr = planHeaderFromRzfp(header);
    const uint64_t leaves_per_sb = rzfpTotalLeafsPerSuper(header);

    std::vector<InternalLeafTask> internal_tasks;

    for (const auto& request : requests) {
        SBTaskPlan sb_plan;
        switch (request.axis) {
            case SliceAxis::X:
                sb_plan = buildSBPlanX(plan_hdr, request.index);
                break;
            case SliceAxis::Y:
                sb_plan = buildSBPlanY(plan_hdr, request.index);
                break;
            case SliceAxis::Z:
                sb_plan = buildSBPlanZ(plan_hdr, request.index);
                break;
        }

        const LeafOp* ops = sb_plan.leaf_ops.data();
        for (const auto& task : sb_plan.tasks) {
            const uint64_t physical_sb =
                computePhysicalSuperblockId(header, task.sb_index);
            for (uint32_t li = 0; li < task.leaf_count; ++li) {
                const LeafOp& op = ops[task.first_leaf + li];
                RoundScatterTarget target;
                target.output = request.output;
                target.op = op;
                target.logical_group = request.logical_group;
                target.request_index = request.request_index;

                plan.logical_leaf_requests++;

                internal_tasks.push_back(
                    {physical_sb, op.morton, target}
                );
            }
        }
    }

    std::unordered_map<uint64_t, size_t> task_map;
    task_map.reserve(internal_tasks.size());

    std::unordered_set<uint64_t> unique_sb_ids;

    for (const auto& internal : internal_tasks) {
        const uint64_t key =
            (internal.physical_sb_id << 16) | internal.morton;
        const auto it = task_map.find(key);
        if (it == task_map.end()) {
            RoundLeafTask leaf_task;
            leaf_task.physical_superblock_id = internal.physical_sb_id;
            leaf_task.morton = internal.morton;
            leaf_task.targets.push_back(internal.target);
            const size_t idx = plan.unique_leaf_tasks.size();
            plan.unique_leaf_tasks.push_back(std::move(leaf_task));
            task_map.emplace(key, idx);
            unique_sb_ids.insert(internal.physical_sb_id);
        } else {
            plan.unique_leaf_tasks[it->second].targets.push_back(
                internal.target
            );
            plan.duplicate_leaf_requests++;
        }
    }

    plan.unique_leaf_count = plan.unique_leaf_tasks.size();
    plan.unique_superblock_count = unique_sb_ids.size();

    const auto checkpoints = buildPrefixCheckpoints(
        unique_sb_ids,
        descriptors,
        leaves_per_sb
    );

    computeTaskOffsets(
        plan.unique_leaf_tasks,
        descriptors,
        superblock_index,
        leaves_per_sb,
        checkpoints
    );

    plan.intervals = buildIntervals(
        plan.unique_leaf_tasks,
        window_config
    );

    for (const auto& interval : plan.intervals) {
        plan.planned_read_bytes += interval.size;
    }
    plan.planned_pread_calls = plan.intervals.size();

    plan.independent_requested_bytes = 0;
    for (const auto& internal : internal_tasks) {
        const uint64_t descriptor_base =
            internal.physical_sb_id * leaves_per_sb;
        plan.independent_requested_bytes += static_cast<uint64_t>(
            descriptorSize(descriptors[descriptor_base + internal.morton])
        );
    }

    plan.eliminated_read_bytes =
        plan.independent_requested_bytes > plan.planned_read_bytes
            ? plan.independent_requested_bytes - plan.planned_read_bytes
            : 0;

    return plan;
}

bool validateRoundPlan(
    const RzfpRoundPlan& plan,
    uint64_t payload_start,
    uint64_t payload_end,
    std::string* error
) {
    if (error) error->clear();

    if (plan.unique_leaf_tasks.empty() && plan.intervals.empty()) {
        return true;
    }

    uint64_t prev_end = 0;
    bool first = true;
    for (const auto& interval : plan.intervals) {
        if (interval.size == 0) {
            if (error) *error = "interval has zero size";
            return false;
        }
        if (interval.task_count == 0) {
            if (error) *error = "interval has zero task count";
            return false;
        }
        if (interval.offset < payload_start) {
            if (error) *error = "interval offset before payload start";
            return false;
        }
        const uint64_t end = interval.offset + interval.size;
        if (end > payload_end) {
            if (error) *error = "interval extends past payload end";
            return false;
        }
        if (!first && interval.offset < prev_end) {
            if (error) *error = "intervals overlap";
            return false;
        }
        prev_end = end;
        first = false;
    }

    for (const auto& task : plan.unique_leaf_tasks) {
        const uint64_t leaf_size = static_cast<uint64_t>(task.record_size);
        if (task.file_offset < payload_start) {
            if (error) *error = "task file_offset before payload start";
            return false;
        }
        if (task.file_offset + leaf_size > payload_end) {
            if (error) *error = "task extends past payload end";
            return false;
        }
    }

    return true;
}

} // namespace erwt3d
