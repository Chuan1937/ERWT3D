#pragma once

#include "erwt3d/format.hpp"
#include "erwt3d/sb_hdd.hpp"
#include "erwt3d/ssd/ssd_config.hpp"
#include "erwt3d/ssd/ssd_extent_planner.hpp"

#include <cstdint>
#include <vector>

namespace erwt3d {

bool executeSBBatchSSD(
    int fd,
    const SBBatchPlan& batch,
    const ERWT3DHeader& header,
    float* const* outputs,
    int readThreads,
    int decodeThreads,
    const SSDReadConfig& ssdCfg,
    SBBatchProfile* profile);

struct CompressedSBInfo {
    uint64_t sb_idx;
    uint64_t compressed_offset;
    uint32_t compressed_size;
    uint8_t is_compressed;
    std::vector<size_t> scatter_indices;
};

bool executeCompressedBatchSSD(
    int fd,
    const ERWT3DHeader& header,
    const std::vector<CompressedSBInfo>& sortedCompressedSBs,
    const SBBatchPlan& batch,
    float* const* outputs,
    int readThreads,
    int decodeThreads,
    const SSDReadConfig& ssdCfg,
    SBBatchProfile* profile);

} // namespace erwt3d
