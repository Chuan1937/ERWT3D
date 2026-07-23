#include "erwt3d/ssd/ssd_extent_planner.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace erwt3d {
namespace {

struct DedupKey {
    uint64_t slab_id;
    uint16_t morton;
    bool is_xplane;

    bool operator==(const DedupKey& o) const {
        return slab_id == o.slab_id && morton == o.morton && is_xplane == o.is_xplane;
    }
};

struct DedupKeyHash {
    size_t operator()(const DedupKey& k) const {
        return static_cast<size_t>(k.slab_id ^ (static_cast<uint64_t>(k.morton) << 32) ^
                                    (k.is_xplane ? 0x8000000000000000ULL : 0));
    }
};

static constexpr uint64_t kIOCostNS = 10000;
static constexpr uint64_t kBandwidthNS_GBPS = 1;

inline uint64_t gapPenaltyNS(uint64_t gapBytes, double bw_mb_s) {
    double bw_gbps = bw_mb_s / 1000.0;
    double sec = static_cast<double>(gapBytes) / (bw_gbps * 1e9);
    return static_cast<uint64_t>(sec * 1e9);
}

} // anonymous namespace

SSDExtentPlan buildSSDExtentPlan(
    std::vector<SSDLeafRequest> requests,
    const SSDExtentPlanConfig& config)
{
    SSDExtentPlan plan;
    plan.logical_requests = requests.size();
    plan.requested_record_bytes = 0;
    for (const auto& r : requests) {
        plan.requested_record_bytes += r.record_size;
    }

    if (requests.empty()) return plan;

    std::sort(requests.begin(), requests.end(),
        [](const SSDLeafRequest& a, const SSDLeafRequest& b) {
            if (a.file_offset != b.file_offset) return a.file_offset < b.file_offset;
            if (a.record_size != b.record_size) return a.record_size < b.record_size;
            return a.leaf_id < b.leaf_id;
        });

    std::vector<SSDLeafRequest> deduped;
    deduped.reserve(requests.size());

    {
        std::unordered_map<DedupKey, size_t, DedupKeyHash> seen;

        for (auto& req : requests) {
            DedupKey key{req.superblock_id, req.morton, req.is_xplane};

            auto it = seen.find(key);
            if (it != seen.end()) {
                plan.duplicate_requests++;
                plan.eliminated_record_bytes += req.record_size;
                continue;
            }

            seen[key] = deduped.size();
            deduped.push_back(std::move(req));
        }
    }

    plan.unique_leaves = deduped.size();
    {
        uint64_t actualBytes = 0;
        for (const auto& r : deduped) actualBytes += r.record_size;
        plan.eliminated_record_bytes = plan.requested_record_bytes > actualBytes
            ? plan.requested_record_bytes - actualBytes : 0;
    }

    std::vector<SSDLeafRequest> deduped2;
    deduped2.reserve(deduped.size());
    for (auto& r : deduped) {
        if (r.record_size > 0) {
            deduped2.push_back(std::move(r));
        }
    }
    deduped = std::move(deduped2);

    plan.leaves = std::move(deduped);
    plan.extent_count_before_merge = plan.leaves.size();

    if (plan.leaves.empty()) return plan;

    const double bw_mb_s = config.estimated_bandwidth_mb_s;
    const uint64_t ioCostNS = static_cast<uint64_t>(config.io_submission_cost_us * 1000.0);
    const uint64_t maxGap = config.max_gap_bytes;

    size_t i = 0;
    while (i < plan.leaves.size()) {
        const SSDLeafRequest& first = plan.leaves[i];
        uint64_t extentStart = first.file_offset;
        uint64_t extentEnd = first.file_offset + first.record_size;
        size_t extentLeafCount = 1;

        size_t j = i + 1;
        while (j < plan.leaves.size()) {
            const SSDLeafRequest& next = plan.leaves[j];
            uint64_t gap = (next.file_offset > extentEnd)
                ? (next.file_offset - extentEnd) : 0;

            if (gap == 0) {
                extentEnd = std::max(extentEnd, next.file_offset + next.record_size);
                extentLeafCount++;
                j++;
                continue;
            }

            if (gap > maxGap) break;

            uint64_t gapCostNS = gapPenaltyNS(gap, bw_mb_s);
            if (gapCostNS < ioCostNS) {
                extentEnd = next.file_offset + next.record_size;
                plan.merged_gap_bytes += gap;
                extentLeafCount++;
                j++;
            } else {
                break;
            }
        }

        uint64_t extentSize = extentEnd - extentStart;
        if (extentSize > config.read_window_bytes) {
            extentEnd = extentStart + config.read_window_bytes;
            extentSize = config.read_window_bytes;
            size_t k = j - 1;
            while (k > i && plan.leaves[k].file_offset + plan.leaves[k].record_size > extentEnd) {
                k--;
            }
            if (k > i) {
                j = k + 1;
                extentLeafCount = j - i;
                extentEnd = plan.leaves[j - 1].file_offset + plan.leaves[j - 1].record_size;
                extentSize = extentEnd - extentStart;
            }
        }

        SSDExtent ext;
        ext.offset = extentStart;
        ext.size = extentSize;
        ext.first_leaf = i;
        ext.leaf_count = extentLeafCount;
        plan.extents.push_back(ext);
        plan.planned_read_bytes += extentSize;

        i = j;
    }

    plan.pread_calls = plan.extents.size();
    plan.read_amplification = plan.requested_record_bytes > 0
        ? static_cast<double>(plan.planned_read_bytes) / plan.requested_record_bytes
        : 1.0;
    plan.read_amplification_numerator = plan.planned_read_bytes;

    return plan;
}

} // namespace erwt3d
