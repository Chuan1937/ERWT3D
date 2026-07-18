#pragma once

#include "erwt3d/rzfp_reader.hpp"

#include <cstdint>
#include <string>

namespace erwt3d {

struct StrategyCostInput {
    uint64_t selective_bytes = 0;
    uint64_t selective_preads = 0;
    uint64_t whole_bytes = 0;
    uint64_t whole_preads = 0;
    uint64_t fullscan_bytes = 0;
    uint64_t fullscan_preads = 0;
    uint64_t decoded_records = 0;

    double sequential_mb_s = 0.0;
    double seek_ms = 0.0;
    double decode_records_per_second = 500000.0;
};

struct StrategyEstimate {
    RzfpReadStrategy strategy = RzfpReadStrategy::SelectiveLeaf;
    uint64_t read_bytes = 0;
    uint64_t pread_calls = 0;
    uint64_t decoded_records = 0;
    double io_seconds = 0.0;
    double seek_seconds = 0.0;
    double decode_seconds = 0.0;
    double total_seconds = 0.0;
    bool allowed = true;
    std::string rejection_reason;
};

struct StrategyDecision {
    RzfpReadStrategy selected = RzfpReadStrategy::SelectiveLeaf;
    StrategyEstimate selective;
    StrategyEstimate whole;
    StrategyEstimate fullscan;
    bool uncertain = false;
    bool slow_device_fullscan_blocked = false;
    std::string reason;
};

StrategyDecision chooseAdaptiveStrategyFromCosts(
    const StrategyCostInput& input,
    const RzfpAdaptiveConfig& config
);

void applyObservedBandwidth(
    StrategyDecision& decision,
    double observed_mb_s,
    const RzfpAdaptiveConfig& config
);

} // namespace erwt3d
