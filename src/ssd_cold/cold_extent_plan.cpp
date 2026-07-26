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

    std::vector<size_t> sortedIdx(slabs.size());
    for (size_t i = 0; i < slabs.size(); ++i) sortedIdx[i] = i;
    std::sort(sortedIdx.begin(), sortedIdx.end(), [&](size_t a, size_t b) {
        return slabs[a].file_offset < slabs[b].file_offset;
    });

    const double bwMbS = config.estimated_bandwidth_mb_s;
    const uint64_t ioSubmitCostNS = static_cast<uint64_t>(config.io_submission_cost_us * 1000.0);
    const uint64_t maxGap = config.max_gap_bytes;
    const uint64_t maxExtent = config.max_extent_bytes;

    if (config.one_extent_per_slab) {
        for (size_t idx : sortedIdx) {
            const auto& s = slabs[idx];
            ColdExtent ext;
            ext.fd = s.fd;
            ext.file_offset = s.file_offset;
            ext.size = s.slab_bytes;
            ext.slab_indices.push_back(idx);
            plan.extents.push_back(std::move(ext));
            plan.planned_read_bytes += s.slab_bytes;
        }
        plan.read_amplification = 1.0;
        return plan;
    }

    size_t i = 0;
    while (i < sortedIdx.size()) {
        const size_t firstIdx = sortedIdx[i];
        const auto& first = slabs[firstIdx];

        ColdExtent ext;
        ext.fd = first.fd;
        ext.file_offset = first.file_offset;
        ext.size = first.slab_bytes;
        ext.slab_indices.push_back(firstIdx);

        if (first.slab_bytes > maxExtent) {
            plan.extents.push_back(std::move(ext));
            ++i;
            continue;
        }

        uint64_t extentEnd = first.file_offset + first.slab_bytes;

        size_t j = i + 1;
        while (j < sortedIdx.size()) {
            const size_t nextIdx = sortedIdx[j];
            const auto& next = slabs[nextIdx];

            if (!config.cross_section_merge && next.source != first.source) break;
            if (!config.cross_fd_merge && next.fd != first.fd) break;

            uint64_t gap = (next.file_offset > extentEnd)
                ? (next.file_offset - extentEnd) : 0;

            if (gap > maxGap) break;

            if (gap > 0) {
                uint64_t gapCostNS = gapPenaltyNS(gap, bwMbS);
                if (gapCostNS >= ioSubmitCostNS) break;
            }

            const uint64_t nextEnd = next.file_offset + next.slab_bytes;
            const uint64_t newEnd = std::max(extentEnd, nextEnd);
            const uint64_t newSize = newEnd - ext.file_offset;

            if (newSize > maxExtent) break;

            extentEnd = newEnd;
            ext.size = newSize;
            if (gap > 0) plan.gap_bytes += gap;
            ext.slab_indices.push_back(nextIdx);
            ++j;
        }

        plan.extents.push_back(std::move(ext));
        i = j;
    }

    uint64_t totalReadBytes = 0;
    for (const auto& e : plan.extents) totalReadBytes += e.size;
    plan.planned_read_bytes = totalReadBytes;

    uint64_t totalSlabBytes = 0;
    for (const auto& s : slabs) totalSlabBytes += s.slab_bytes;

    plan.read_amplification = totalSlabBytes > 0
        ? static_cast<double>(totalReadBytes) / static_cast<double>(totalSlabBytes)
        : 1.0;

    return plan;
}

bool validateExtentCoverage(
    const std::vector<ColdSlabRequest>& slabs,
    const ColdExtentPlan& extentPlan,
    std::string& error)
{
    std::vector<uint32_t> coverage(slabs.size(), 0);

    for (const auto& ext : extentPlan.extents) {
        for (size_t idx : ext.slab_indices) {
            if (idx >= slabs.size()) {
                error = "extent references out-of-range slab index " + std::to_string(idx);
                return false;
            }

            const auto& slab = slabs[idx];

            if (slab.fd != ext.fd) {
                error = "slab " + std::to_string(idx) + " fd mismatch";
                return false;
            }

            if (slab.file_offset < ext.file_offset) {
                error = "slab " + std::to_string(idx) + " starts before extent";
                return false;
            }

            uint64_t rel = slab.file_offset - ext.file_offset;
            if (rel > ext.size || slab.slab_bytes > ext.size - rel) {
                error = "extent does not fully cover slab " + std::to_string(idx);
                return false;
            }

            ++coverage[idx];
        }
    }

    for (size_t i = 0; i < coverage.size(); ++i) {
        if (coverage[i] == 0) {
            error = "slab " + std::to_string(i) + " is not covered by any extent";
            return false;
        }
        if (coverage[i] > 1) {
            error = "slab " + std::to_string(i) + " is covered by " + std::to_string(coverage[i]) + " extents";
            return false;
        }
    }

    return true;
}

} // namespace ssd_cold
} // namespace erwt3d
