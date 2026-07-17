#pragma once

#include "erwt3d/relative_error.hpp"

#include <cstdint>
#include <vector>

namespace erwt3d {

enum class RzfpXPlaneCodec : uint8_t {
    RawFloat32 = 0,
    ConstantZero = 1,
    ConstantValue = 2,
    ZfpAccuracy = 3,
    ZfpAccuracyExceptions = 4,
    ZfpPrecision = 5,
    ZfpPrecisionExceptions = 6,
};

struct RzfpXPlaneCodecConfig {
    RelativeErrorConfig error;
    std::vector<uint8_t> precisions{12, 14, 16, 18, 20, 22, 24};
    bool fast_accuracy_only = false;
    bool try_precision_exceptions = true;
};

// Encode one YZ-plane (ny*nz floats, y fastest) into a single record.
std::vector<uint8_t> encodeXPlane2D(
    const float* plane,
    uint64_t ny,
    uint64_t nz,
    const RzfpXPlaneCodecConfig& config
);

// Decode a record produced by encodeXPlane2D into a ny*nz float plane.
bool decodeXPlane2D(
    const uint8_t* record,
    size_t record_size,
    float* plane,
    uint64_t ny,
    uint64_t nz
);

} // namespace erwt3d
