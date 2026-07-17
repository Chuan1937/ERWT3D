#include "erwt3d/rzfp_strategy.hpp"

#include <cstdint>
#include <iostream>

namespace {

int failures = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << "FAIL: " << #condition \
                  << " at line " << __LINE__ << "\n"; \
        ++failures; \
    } \
} while (false)

constexpr uint64_t MiB = 1024ULL * 1024ULL;
constexpr uint64_t GiB = 1024ULL * MiB;

void testSlowHddBlocksLargeFullscan() {
    erwt3d::StrategyCostInput input;
    input.selective_bytes = 6ULL * GiB;
    input.selective_preads = 100;
    input.whole_bytes = 5ULL * GiB;
    input.whole_preads = 20;
    input.fullscan_bytes = 21ULL * GiB;
    input.fullscan_preads = 42;
    input.decoded_records = 100000;
    input.sequential_mb_s = 66.0;
    input.seek_ms = 12.0;

    erwt3d::RzfpAdaptiveConfig config;
    const auto decision =
        erwt3d::chooseAdaptiveStrategyFromCosts(input, config);

    CHECK(!decision.fullscan.allowed);
    CHECK(decision.slow_device_fullscan_blocked);
    CHECK(decision.selected != erwt3d::RzfpReadStrategy::FullPayloadScan);
    CHECK(decision.selected == erwt3d::RzfpReadStrategy::WholeSuperblock);
}

void testTimeLimitMayBeExceededWithRealAdvantage() {
    erwt3d::StrategyCostInput input;
    input.selective_bytes = 30ULL * GiB;
    input.selective_preads = 100;
    input.whole_bytes = 28ULL * GiB;
    input.whole_preads = 50;
    input.fullscan_bytes = 21ULL * GiB;
    input.fullscan_preads = 42;
    input.decoded_records = 100000;
    input.sequential_mb_s = 150.0;
    input.seek_ms = 9.0;

    erwt3d::RzfpAdaptiveConfig config;
    config.max_fullscan_seconds = 120.0;
    config.fullscan_min_advantage = 0.20;

    const auto decision =
        erwt3d::chooseAdaptiveStrategyFromCosts(input, config);

    CHECK(decision.fullscan.total_seconds > 120.0);
    CHECK(decision.fullscan.allowed);
    CHECK(decision.selected == erwt3d::RzfpReadStrategy::FullPayloadScan);
}

void testFastHddMaySelectFullscan() {
    erwt3d::StrategyCostInput input;
    input.selective_bytes = 30ULL * GiB;
    input.selective_preads = 100;
    input.whole_bytes = 29ULL * GiB;
    input.whole_preads = 50;
    input.fullscan_bytes = 21ULL * GiB;
    input.fullscan_preads = 42;
    input.decoded_records = 100000;
    input.sequential_mb_s = 300.0;
    input.seek_ms = 7.0;

    erwt3d::RzfpAdaptiveConfig config;
    const auto decision =
        erwt3d::chooseAdaptiveStrategyFromCosts(input, config);

    CHECK(decision.fullscan.allowed);
    CHECK(decision.selected == erwt3d::RzfpReadStrategy::FullPayloadScan);
}

void testHysteresisPrefersLowerReadVolume() {
    erwt3d::StrategyCostInput input;
    input.selective_bytes = 100ULL * MiB;
    input.selective_preads = 0;
    input.whole_bytes = 80ULL * MiB;
    input.whole_preads = 25;
    input.fullscan_bytes = 2ULL * GiB;
    input.fullscan_preads = 4;
    input.sequential_mb_s = 100.0;
    input.seek_ms = 10.0;

    erwt3d::RzfpAdaptiveConfig config;
    config.strategy_switch_margin = 0.15;
    const auto decision =
        erwt3d::chooseAdaptiveStrategyFromCosts(input, config);

    CHECK(decision.uncertain);
    CHECK(decision.selected == erwt3d::RzfpReadStrategy::WholeSuperblock);
}

void testClearWinnerIsSelected() {
    erwt3d::StrategyCostInput input;
    input.selective_bytes = 80ULL * MiB;
    input.selective_preads = 0;
    input.whole_bytes = 120ULL * MiB;
    input.whole_preads = 0;
    input.fullscan_bytes = 2ULL * GiB;
    input.fullscan_preads = 4;
    input.sequential_mb_s = 100.0;
    input.seek_ms = 10.0;

    erwt3d::RzfpAdaptiveConfig config;
    const auto decision =
        erwt3d::chooseAdaptiveStrategyFromCosts(input, config);

    CHECK(!decision.uncertain);
    CHECK(decision.selected == erwt3d::RzfpReadStrategy::SelectiveLeaf);
}

void testPilotRecalculatesIoSeparately() {
    erwt3d::StrategyCostInput input;
    input.selective_bytes = 100ULL * MiB;
    input.selective_preads = 100;
    input.whole_bytes = 180ULL * MiB;
    input.whole_preads = 1;
    input.fullscan_bytes = 2ULL * GiB;
    input.fullscan_preads = 4;
    input.sequential_mb_s = 80.0;
    input.seek_ms = 10.0;

    erwt3d::RzfpAdaptiveConfig config;
    auto decision = erwt3d::chooseAdaptiveStrategyFromCosts(input, config);
    CHECK(decision.selected == erwt3d::RzfpReadStrategy::SelectiveLeaf);

    erwt3d::applyObservedBandwidth(decision, 400.0, config);
    CHECK(decision.selected == erwt3d::RzfpReadStrategy::WholeSuperblock);
}

void testPilotCannotBypassSlowDeviceBlock() {
    erwt3d::StrategyCostInput input;
    input.selective_bytes = 6ULL * GiB;
    input.selective_preads = 100;
    input.whole_bytes = 5ULL * GiB;
    input.whole_preads = 20;
    input.fullscan_bytes = 21ULL * GiB;
    input.fullscan_preads = 42;
    input.sequential_mb_s = 66.0;
    input.seek_ms = 12.0;

    erwt3d::RzfpAdaptiveConfig config;
    auto decision = erwt3d::chooseAdaptiveStrategyFromCosts(input, config);
    CHECK(decision.slow_device_fullscan_blocked);

    erwt3d::applyObservedBandwidth(decision, 500.0, config);
    CHECK(!decision.fullscan.allowed);
    CHECK(decision.selected != erwt3d::RzfpReadStrategy::FullPayloadScan);
}

} // namespace

int main() {
    std::cout << "test_strategy_cost\n";

    testSlowHddBlocksLargeFullscan();
    testTimeLimitMayBeExceededWithRealAdvantage();
    testFastHddMaySelectFullscan();
    testHysteresisPrefersLowerReadVolume();
    testClearWinnerIsSelected();
    testPilotRecalculatesIoSeparately();
    testPilotCannotBypassSlowDeviceBlock();

    if (failures == 0) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    }

    std::cerr << failures << " TESTS FAILED\n";
    return 1;
}
