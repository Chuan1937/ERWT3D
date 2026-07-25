#include "erwt3d/ssd_cold/cold_extent_plan.hpp"

#include <algorithm>
#include <cmath>

namespace erwt3d {
namespace ssd_cold {

namespace {

static uint64_t gapPenaltyNS(uint64_t gapBytes, double bwMbS) {
    if (bwMbS <= 0) bwMbS = 3000.0;
    double gbPerSec = bwMbS / 1000.0;
    double sec = static_cast<double>(gapBytes) / (gbPerSec * 1e9);
    return static_cast<uint64_t>(sec * 1e9);
}

} // anonymous namespace

ColdExtentPlan buildColdExtentPlan(
    const std::vector<ColdSlabRequest>& slabs,
    const ColdExtentPlanConfig& config)
{
    ColdExtentPlan plan;
    plan.slabs = &slabs;
    plan.extent_count_before_merge = slabs.size();

    if (slabs.empty()) return plan;

    for (const auto& s : slabs) {
        plan.planned_read_bytes += s.slab_bytes;
    }

    std::vector<size_t> sortedIdx(slabs.size());
    for (size_t i = 0; i < slabs.size(); ++i) sortedIdx[i] = i;
    std::sort(sortedIdx.begin(), sortedIdx.end(), [&](size_t a, size_t b) {
        return slabs[a].file_offset < slabs[b].file_offset;
    });

    const double bwMbS = config.estimated_bandwidth_mb_s;
    const uint64_t ioSubmitCostNS = static_cast<uint64_t>(config.io_submission_cost_us * 1000.0);
    const uint64_t maxGap = config.max_gap_bytes;
    const uint64_t maxExtent = config.max_extent_bytes;

    size_t i = 0;
    while (i < sortedIdx.size()) {
        const auto& first = slabs[sortedIdx[i]];
        uint64_t extentStart = first.file_offset;
        uint64_t extentEnd = first.file_offset + first.slab_bytes;
        size_t extentCount = 1;
        int extentFd = first.fd;

        size_t j = i + 1;
        while (j < sortedIdx.size()) {
            const auto& next = slabs[sortedIdx[j]];

            if (!config.cross_section_merge && next.source != first.source) break;
            if (!config.cross_fd_merge && next.fd != extentFd) break;

            uint64_t gap = (next.file_offset > extentEnd)
                ? (next.file_offset - extentEnd) : 0;

            if (gap == 0) {
                extentEnd = std::max(extentEnd, next.file_offset + next.slab_bytes);
                extentCount++;
                j++;
                continue;
            }

            if (gap > maxGap) break;

            uint64_t gapCostNS = gapPenaltyNS(gap, bwMbS);
            if (gapCostNS >= ioSubmitCostNS) break;

            uint64_t newEnd = next.file_offset + next.slab_bytes;
            uint64_t newSize = newEnd - extentStart;
            if (newSize > maxExtent) break;

            extentEnd = newEnd;
            plan.gap_bytes += gap;
            extentCount++;
            j++;
        }

        uint64_t extentSize = extentEnd - extentStart;
        if (extentSize > maxExtent) {
            extentEnd = extentStart + maxExtent;
            extentSize = maxExtent;
            size_t k = j - 1;
            while (k > i && slabs[sortedIdx[k]].file_offset +
                   slabs[sortedIdx[k]].slab_bytes > extentEnd) {
                k--;
            }
            if (k > i) {
                j = k + 1;
                extentCount = j - i;
                extentEnd = slabs[sortedIdx[j - 1]].file_offset +
                            slabs[sortedIdx[j - 1]].slab_bytes;
                extentSize = extentEnd - extentStart;
            }
        }

        ColdExtent ext;
        ext.file_offset = extentStart;
        ext.size = extentSize;
        ext.first_slab = i;
        ext.slab_count = extentCount;
        ext.fd = extentFd;
        plan.extents.push_back(ext);

        i = j;
    }

    uint64_t totalRecordBytes = 0;
    for (const auto& s : slabs) totalRecordBytes += s.slab_bytes;

    uint64_t totalReadBytes = 0;
    for (const auto& e : plan.extents) totalReadBytes += e.size;
    plan.planned_read_bytes = totalReadBytes;

    plan.read_amplification = totalRecordBytes > 0
        ? static_cast<double>(totalReadBytes) / static_cast<double>(totalRecordBytes)
        : 1.0;

    return plan;
}

} // namespace ssd_cold
} // namespace erwt3d
