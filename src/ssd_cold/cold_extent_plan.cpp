#include "erwt3d/ssd_cold/cold_extent_plan.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace erwt3d {
namespace ssd_cold {

namespace {

static uint64_t gapPenaltyNS(uint64_t gapBytes, double bwMbS) {
    if (bwMbS <= 0) bwMbS = 1000.0;
    double gbPerSec = bwMbS / 1000.0;
    double sec = static_cast<double>(gapBytes) / (gbPerSec * 1e9);
    return static_cast<uint64_t>(sec * 1e9);
}

} // anonymous namespace

ColdExtentPlan buildColdExtentPlan(
    const std::vector<ColdLeafRecord>& records,
    const ColdExtentPlanConfig& config)
{
    ColdExtentPlan plan;
    plan.records = &records;
    plan.extent_count_before_merge = records.size();

    if (records.empty()) return plan;

    for (const auto& r : records) {
        plan.planned_read_bytes += r.record_size;
    }

    std::vector<size_t> sortedIdx(records.size());
    for (size_t i = 0; i < records.size(); ++i) sortedIdx[i] = i;
    std::sort(sortedIdx.begin(), sortedIdx.end(), [&](size_t a, size_t b) {
        return records[a].file_offset < records[b].file_offset;
    });

    const double bwMbS = config.estimated_bandwidth_mb_s;
    const uint64_t ioSubmitCostNS = static_cast<uint64_t>(config.io_submission_cost_us * 1000.0);
    const uint64_t maxGap = config.max_gap_bytes;
    const uint64_t maxExtent = config.max_extent_bytes;

    size_t i = 0;
    while (i < sortedIdx.size()) {
        const auto& first = records[sortedIdx[i]];
        uint64_t extentStart = first.file_offset;
        uint64_t extentEnd = first.file_offset + first.record_size;
        size_t extentCount = 1;

        size_t j = i + 1;
        while (j < sortedIdx.size()) {
            const auto& next = records[sortedIdx[j]];

            if (!config.cross_section_merge && next.source != first.source) break;

            uint64_t gap = (next.file_offset > extentEnd) ? (next.file_offset - extentEnd) : 0;

            if (gap == 0) {
                extentEnd = std::max(extentEnd, next.file_offset + next.record_size);
                extentCount++;
                j++;
                continue;
            }

            if (gap > maxGap) break;

            uint64_t gapCostNS = gapPenaltyNS(gap, bwMbS);
            if (gapCostNS >= ioSubmitCostNS) break;

            uint64_t newEnd = next.file_offset + next.record_size;
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
            while (k > i && records[sortedIdx[k]].file_offset +
                   records[sortedIdx[k]].record_size > extentEnd) {
                k--;
            }
            if (k > i) {
                j = k + 1;
                extentCount = j - i;
                extentEnd = records[sortedIdx[j - 1]].file_offset +
                            records[sortedIdx[j - 1]].record_size;
                extentSize = extentEnd - extentStart;
            }
        }

        ColdExtent ext;
        ext.file_offset = extentStart;
        ext.size = extentSize;
        ext.first_record = i;
        ext.record_count = extentCount;
        plan.extents.push_back(ext);

        i = j;
    }

    uint64_t totalRecordBytes = 0;
    for (const auto& r : records) totalRecordBytes += r.record_size;

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
