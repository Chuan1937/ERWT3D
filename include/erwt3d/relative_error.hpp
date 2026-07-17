#pragma once

#include <cstddef>
#include <cstdint>

namespace erwt3d {

enum class RelativeErrorPolicy : uint8_t {
    Strict,
    Legacy,
};

struct RelativeErrorConfig {
    double contest_bound = 1e-3;
    double internal_bound = 7.5e-4;
    double legacy_zero_abs_tol = 1e-6;
    RelativeErrorPolicy policy = RelativeErrorPolicy::Strict;
};

struct PointErrorResult {
    bool passed = false;
    double absolute_error = 0.0;
    double relative_error = 0.0;
};

struct BlockErrorStats {
    bool passed = true;
    uint32_t valid_count = 0;
    uint32_t violation_count = 0;

    double max_absolute_error = 0.0;
    double max_relative_error = 0.0;
};

PointErrorResult checkPointwiseError(
    float original,
    float reconstructed,
    const RelativeErrorConfig& config
);

BlockErrorStats checkBlockError(
    const float* original,
    const float* reconstructed,
    size_t count,
    uint64_t valid_mask,
    const RelativeErrorConfig& config
);

} // namespace erwt3d
