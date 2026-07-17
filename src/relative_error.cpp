#include "erwt3d/relative_error.hpp"

#include <cmath>
#include <limits>

namespace erwt3d {

PointErrorResult checkPointwiseError(
    float original,
    float reconstructed,
    const RelativeErrorConfig& config
) {
    PointErrorResult result;

    if (std::isnan(original)) {
        result.passed = std::isnan(reconstructed);
        return result;
    }

    if (std::isinf(original)) {
        result.passed =
            std::isinf(reconstructed) &&
            std::signbit(original) == std::signbit(reconstructed);
        return result;
    }

    if (!std::isfinite(reconstructed)) {
        result.passed = false;
        return result;
    }

    const double ref = static_cast<double>(original);
    const double value = static_cast<double>(reconstructed);
    result.absolute_error = std::abs(value - ref);

    if (original == 0.0f) {
        result.relative_error =
            result.absolute_error == 0.0 ? 0.0
                                          : std::numeric_limits<double>::infinity();
        if (config.policy == RelativeErrorPolicy::Legacy) {
            result.passed = result.absolute_error <= config.legacy_zero_abs_tol;
        } else {
            result.passed = reconstructed == 0.0f;
        }
        return result;
    }

    result.relative_error = result.absolute_error / std::abs(ref);

    if (config.policy == RelativeErrorPolicy::Legacy) {
        const double abs_ref = std::abs(ref);
        if (abs_ref <= config.legacy_zero_abs_tol) {
            result.passed = result.absolute_error <= config.legacy_zero_abs_tol;
        } else {
            result.passed = result.relative_error < config.contest_bound;
        }
    } else {
        result.passed = result.relative_error < config.contest_bound;
    }

    return result;
}

BlockErrorStats checkBlockError(
    const float* original,
    const float* reconstructed,
    size_t count,
    uint64_t valid_mask,
    const RelativeErrorConfig& config
) {
    BlockErrorStats stats;

    for (size_t i = 0; i < count; ++i) {
        if ((valid_mask & (uint64_t{1} << i)) == 0) {
            continue;
        }

        ++stats.valid_count;
        const auto point = checkPointwiseError(original[i], reconstructed[i], config);

        stats.max_absolute_error = std::max(stats.max_absolute_error, point.absolute_error);
        stats.max_relative_error = std::max(stats.max_relative_error, point.relative_error);

        if (!point.passed) {
            ++stats.violation_count;
            stats.passed = false;
        }
    }

    return stats;
}

} // namespace erwt3d
