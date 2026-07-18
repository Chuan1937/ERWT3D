#pragma once

#include "rzfp_format.hpp"
#include "rzfp_codec.hpp"
#include "sb_plan.hpp"
#include "sb_hdd.hpp"
#include "slice.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace erwt3d {

struct RoundSliceRequest {
    SliceAxis axis = SliceAxis::X;
    uint64_t index = 0;
    float* output = nullptr;

    uint32_t logical_group = 0;
    uint32_t request_index = 0;
};

struct RoundScatterTarget {
    float* output = nullptr;
    LeafOp op{};
    uint32_t logical_group = 0;
    uint32_t request_index = 0;
};

struct RoundLeafTask {
    uint64_t physical_superblock_id = 0;
    uint16_t morton = 0;

    uint64_t file_offset = 0;
    uint16_t record_size = 0;
    RzfpLeafCodec codec = RzfpLeafCodec::RawFloat32;

    std::vector<RoundScatterTarget> targets;
};

struct RoundReadInterval {
    uint64_t offset = 0;
    uint64_t size = 0;

    size_t first_task = 0;
    size_t task_count = 0;
};

struct RzfpRoundPlan {
    std::vector<RoundLeafTask> unique_leaf_tasks;
    std::vector<RoundReadInterval> intervals;

    uint64_t logical_leaf_requests = 0;
    uint64_t unique_leaf_count = 0;
    uint64_t duplicate_leaf_requests = 0;

    uint64_t independent_requested_bytes = 0;
    uint64_t planned_read_bytes = 0;
    uint64_t eliminated_read_bytes = 0;

    uint64_t unique_superblock_count = 0;
    uint64_t planned_pread_calls = 0;
};

RzfpRoundPlan buildRzfpRoundPlan(
    const RzfpFileHeader& header,
    const std::vector<RzfpSuperblockIndex>& superblock_index,
    const std::vector<RzfpLeafDescriptor>& descriptors,
    const std::vector<RoundSliceRequest>& requests,
    const HDDReadWindowConfig& window_config
);

bool validateRoundPlan(
    const RzfpRoundPlan& plan,
    uint64_t payload_start,
    uint64_t payload_end,
    std::string* error
);

} // namespace erwt3d
