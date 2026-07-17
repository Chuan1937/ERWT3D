#include "erwt3d/rzfp_strategy.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <sstream>

namespace erwt3d {

namespace {

constexpr uint64_t GiB = 1024ULL * 1024ULL * 1024ULL;

static StrategyEstimate makeEstimate(
    RzfpReadStrategy strategy,
    uint64_t bytes,
    uint64_t preads,
    uint64_t records,
    double sequential_mb_s,
    double seek_ms,
    double decode_records_per_second
) {
    StrategyEstimate estimate;
    estimate.strategy = strategy;
    estimate.read_bytes = bytes;
    estimate.pread_calls = preads;
    estimate.decoded_records = records;

    const double bandwidth = std::max(1.0, sequential_mb_s) *
                             1024.0 * 1024.0;
    const double decodeRate = std::max(1.0, decode_records_per_second);

    estimate.io_seconds = static_cast<double>(bytes) / bandwidth;
    estimate.seek_seconds = static_cast<double>(preads) *
                            std::max(0.0, seek_ms) / 1000.0;
    estimate.decode_seconds = static_cast<double>(records) / decodeRate;
    estimate.total_seconds = estimate.io_seconds +
                             estimate.seek_seconds +
                             estimate.decode_seconds;
    return estimate;
}

static double fullscanAdvantage(
    const StrategyEstimate& fullscan,
    const StrategyEstimate& selective,
    const StrategyEstimate& whole
) {
    const double bestNonFull = std::min(
        selective.total_seconds,
        whole.total_seconds
    );
    if (bestNonFull <= 0.0 || fullscan.total_seconds >= bestNonFull) {
        return 0.0;
    }
    return (bestNonFull - fullscan.total_seconds) / bestNonFull;
}

static void applyFullscanProtection(
    StrategyDecision& decision,
    double sequential_mb_s,
    const RzfpAdaptiveConfig& config
) {
    decision.fullscan.allowed = true;
    decision.fullscan.rejection_reason.clear();
    decision.slow_device_fullscan_blocked = false;

    if (sequential_mb_s < 100.0 &&
        decision.fullscan.read_bytes > 8ULL * GiB) {
        decision.fullscan.allowed = false;
        decision.slow_device_fullscan_blocked = true;
        decision.fullscan.rejection_reason =
            "large full scan disabled on slow device";
        return;
    }

    const double advantage = fullscanAdvantage(
        decision.fullscan,
        decision.selective,
        decision.whole
    );

    if (decision.fullscan.total_seconds >
            config.max_fullscan_seconds &&
        advantage < config.fullscan_min_advantage) {
        decision.fullscan.allowed = false;
        decision.fullscan.rejection_reason =
            "predicted full scan exceeds time limit without required advantage";
        return;
    }

    if (advantage < config.fullscan_min_advantage) {
        decision.fullscan.allowed = false;
        decision.fullscan.rejection_reason =
            "full scan does not beat the best lower-volume strategy by the required margin";
    }
}

static int preferenceRank(RzfpReadStrategy strategy) {
    switch (strategy) {
        case RzfpReadStrategy::WholeSuperblock: return 0;
        case RzfpReadStrategy::SelectiveLeaf: return 1;
        case RzfpReadStrategy::FullPayloadScan: return 2;
        default: return 3;
    }
}

static void selectAllowedStrategy(
    StrategyDecision& decision,
    const RzfpAdaptiveConfig& config
) {
    std::array<const StrategyEstimate*, 3> candidates = {
        &decision.selective,
        &decision.whole,
        &decision.fullscan
    };

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const StrategyEstimate* a, const StrategyEstimate* b) {
            if (a->allowed != b->allowed) return a->allowed > b->allowed;
            if (a->total_seconds != b->total_seconds) {
                return a->total_seconds < b->total_seconds;
            }
            if (a->read_bytes != b->read_bytes) {
                return a->read_bytes < b->read_bytes;
            }
            return preferenceRank(a->strategy) < preferenceRank(b->strategy);
        }
    );

    const StrategyEstimate* best = nullptr;
    const StrategyEstimate* second = nullptr;
    for (const StrategyEstimate* candidate : candidates) {
        if (!candidate->allowed) continue;
        if (!best) best = candidate;
        else if (!second) {
            second = candidate;
            break;
        }
    }

    if (!best) {
        decision.selected = RzfpReadStrategy::SelectiveLeaf;
        decision.uncertain = false;
        decision.reason = "all strategies rejected; forced selective fallback";
        return;
    }

    if (!second) {
        decision.selected = best->strategy;
        decision.uncertain = false;
        decision.reason = "only one strategy remained allowed";
        return;
    }

    const double denominator = std::max(second->total_seconds, 1e-12);
    const double relativeGap =
        (second->total_seconds - best->total_seconds) / denominator;

    if (relativeGap >= config.strategy_switch_margin) {
        decision.selected = best->strategy;
        decision.uncertain = false;
        std::ostringstream reason;
        reason << "predicted winner by " << relativeGap * 100.0
               << "% margin";
        decision.reason = reason.str();
        return;
    }

    decision.uncertain = true;

    // When predictions are close, choose the lower-risk option that reads the
    // least data. Resolve exact byte ties with Whole, Selective, Fullscan.
    const StrategyEstimate* lowerVolume = best;
    for (const StrategyEstimate* candidate : candidates) {
        if (!candidate->allowed) continue;
        if (candidate->read_bytes < lowerVolume->read_bytes ||
            (candidate->read_bytes == lowerVolume->read_bytes &&
             preferenceRank(candidate->strategy) <
                 preferenceRank(lowerVolume->strategy))) {
            lowerVolume = candidate;
        }
    }

    decision.selected = lowerVolume->strategy;
    std::ostringstream reason;
    reason << "prediction gap " << relativeGap * 100.0
           << "% is below hysteresis; selected lower read volume";
    decision.reason = reason.str();
}

static void recalculateForBandwidth(
    StrategyEstimate& estimate,
    double observed_mb_s
) {
    const double bytesPerSecond =
        std::max(1.0, observed_mb_s) * 1024.0 * 1024.0;
    estimate.io_seconds =
        static_cast<double>(estimate.read_bytes) / bytesPerSecond;
    estimate.total_seconds = estimate.io_seconds +
                             estimate.seek_seconds +
                             estimate.decode_seconds;
}

} // namespace

StrategyDecision chooseAdaptiveStrategyFromCosts(
    const StrategyCostInput& input,
    const RzfpAdaptiveConfig& config
) {
    StrategyDecision decision;

    const double sequential = input.sequential_mb_s > 1.0
        ? input.sequential_mb_s
        : 80.0;
    const double seek = input.seek_ms > 0.0
        ? input.seek_ms
        : 12.0;
    const double decodeRate = input.decode_records_per_second > 0.0
        ? input.decode_records_per_second
        : 500000.0;

    decision.selective = makeEstimate(
        RzfpReadStrategy::SelectiveLeaf,
        input.selective_bytes,
        input.selective_preads,
        input.decoded_records,
        sequential,
        seek,
        decodeRate
    );
    decision.whole = makeEstimate(
        RzfpReadStrategy::WholeSuperblock,
        input.whole_bytes,
        input.whole_preads,
        input.decoded_records,
        sequential,
        seek,
        decodeRate
    );
    decision.fullscan = makeEstimate(
        RzfpReadStrategy::FullPayloadScan,
        input.fullscan_bytes,
        input.fullscan_preads,
        input.decoded_records,
        sequential,
        seek,
        decodeRate
    );

    applyFullscanProtection(decision, sequential, config);
    selectAllowedStrategy(decision, config);
    return decision;
}

void applyObservedBandwidth(
    StrategyDecision& decision,
    double observed_mb_s,
    const RzfpAdaptiveConfig& config
) {
    if (observed_mb_s <= 1.0) return;

    recalculateForBandwidth(decision.selective, observed_mb_s);
    recalculateForBandwidth(decision.whole, observed_mb_s);
    recalculateForBandwidth(decision.fullscan, observed_mb_s);

    const bool absoluteSlowBlock = decision.slow_device_fullscan_blocked;
    if (absoluteSlowBlock) {
        decision.fullscan.allowed = false;
        decision.fullscan.rejection_reason =
            "large full scan remains disabled on slow device after pilot";
    } else {
        applyFullscanProtection(decision, observed_mb_s, config);
    }

    selectAllowedStrategy(decision, config);

    std::ostringstream reason;
    reason << decision.reason << "; pilot bandwidth "
           << observed_mb_s << " MB/s";
    decision.reason = reason.str();
}

} // namespace erwt3d
