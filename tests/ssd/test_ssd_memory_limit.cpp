#include "erwt3d/io_profile.hpp"
#include "erwt3d/unified_read_config.hpp"
#include "erwt3d/ssd/ssd_config.hpp"
#include "erwt3d/ssd/ssd_extent_planner.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

bool testConfigDefaults() {
    auto cfg = erwt3d::makeUnifiedConfig(
        erwt3d::IOProfileType::SSD, ".", 8, 2048, 0);

    assert(cfg.io_profile == erwt3d::IOProfileType::SSD);
    assert(cfg.ssd.read_threads >= 1);
    assert(cfg.ssd.decode_threads >= 1);
    assert(cfg.ssd.read_window_bytes > 0);
    assert(cfg.ssd.max_gap_bytes > 0);
    assert(cfg.ssd.buffer_pool_bytes > 0);
    assert(cfg.memory_limit_mib == 2048);
    return true;
}

bool testHddConfigDefaults() {
    auto cfg = erwt3d::makeUnifiedConfig(
        erwt3d::IOProfileType::HDD, ".", 4, 4096, 0);

    assert(cfg.io_profile == erwt3d::IOProfileType::HDD);
    assert(cfg.hdd.read_window_bytes >= 4ULL * 1024 * 1024);
    assert(cfg.hdd.max_gap_bytes > 0);
    return true;
}

bool testExtentPlanMemoryBudget() {
    std::vector<erwt3d::SSDLeafRequest> reqs;
    for (size_t i = 0; i < 1000; ++i) {
        erwt3d::SSDLeafRequest r;
        uint64_t base = 1000000ULL + i * 2000ULL;
        r.file_offset = base;
        r.record_size = 256 + (i % 16) * 32;
        r.superblock_id = i;
        r.morton = 0;
        r.leaf_id = (r.superblock_id << 16) | r.morton;
        reqs.push_back(r);
    }

    erwt3d::SSDExtentPlanConfig cfg;
    cfg.read_window_bytes = 4ULL * 1024;
    cfg.max_gap_bytes = 0;
    cfg.buffer_pool_bytes = 1ULL * 1024 * 1024;

    auto plan = erwt3d::buildSSDExtentPlan(std::move(reqs), cfg);

    for (const auto& e : plan.extents) {
        assert(e.size <= cfg.read_window_bytes);
        assert(e.leaf_count > 0);
    }
    assert(plan.pread_calls == plan.extents.size());
    assert(plan.planned_read_bytes <= plan.requested_record_bytes + plan.extents.size() * cfg.max_gap_bytes);
    return true;
}

} // anonymous namespace

int main() {
    assert(testConfigDefaults());
    assert(testHddConfigDefaults());
    assert(testExtentPlanMemoryBudget());
    printf("SSD memory limit test: PASS\n");
    return 0;
}
