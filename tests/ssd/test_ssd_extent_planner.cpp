#include "erwt3d/ssd/ssd_extent_planner.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

bool testSortAndDedup() {
    std::vector<erwt3d::SSDLeafRequest> reqs;
    for (size_t i = 0; i < 10; ++i) {
        erwt3d::SSDLeafRequest r;
        r.file_offset = (20 - i) * 100ULL;
        r.record_size = 50;
        r.superblock_id = i;
        r.morton = 0;
        r.leaf_id = (r.superblock_id << 16) | r.morton;
        reqs.push_back(r);
    }

    erwt3d::SSDExtentPlanConfig cfg;
    cfg.read_window_bytes = 4096;
    cfg.max_gap_bytes = 0;

    auto plan = erwt3d::buildSSDExtentPlan(std::move(reqs), cfg);

    assert(plan.logical_requests == 10);
    assert(plan.unique_leaves == 10);
    assert(plan.duplicate_requests == 0);

    for (size_t i = 1; i < plan.leaves.size(); ++i) {
        assert(plan.leaves[i].file_offset >= plan.leaves[i - 1].file_offset);
    }
    return true;
}

bool testGapMerge() {
    std::vector<erwt3d::SSDLeafRequest> reqs;
    for (size_t i = 0; i < 5; ++i) {
        erwt3d::SSDLeafRequest r;
        r.file_offset = i * 1000ULL;
        r.record_size = 200;
        r.superblock_id = i;
        r.morton = 0;
        r.leaf_id = (r.superblock_id << 16) | r.morton;
        reqs.push_back(r);
    }

    erwt3d::SSDExtentPlanConfig cfg;
    cfg.read_window_bytes = 10000;
    cfg.max_gap_bytes = 700;

    auto plan = erwt3d::buildSSDExtentPlan(std::move(reqs), cfg);

    assert(plan.extents.size() < 5);
    assert(plan.pread_calls == plan.extents.size());

    uint64_t totalExtentBytes = 0;
    for (const auto& e : plan.extents) {
        totalExtentBytes += e.size;
        assert(e.leaf_count > 0);
        assert(e.first_leaf + e.leaf_count <= plan.leaves.size());
    }
    assert(totalExtentBytes == plan.planned_read_bytes);
    assert(plan.read_amplification >= 1.0);
    return true;
}

bool testCoverage() {
    erwt3d::SSDLeafRequest r;
    r.file_offset = 10000;
    r.record_size = 256;
    r.superblock_id = 1;
    r.morton = 0;
    r.leaf_id = (1ULL << 16) | 0;

    erwt3d::SSDExtentPlanConfig cfg;
    cfg.read_window_bytes = 4096;
    cfg.max_gap_bytes = 0;

    auto plan = erwt3d::buildSSDExtentPlan({r}, cfg);
    assert(plan.leaves.size() == 1);
    assert(plan.extents.size() == 1);
    assert(plan.extents[0].offset == 10000);
    assert(plan.extents[0].size == 256);
    return true;
}

} // anonymous namespace

int main() {
    assert(testSortAndDedup());
    assert(testGapMerge());
    assert(testCoverage());
    printf("SSD extent planner test: PASS\n");
    return 0;
}
