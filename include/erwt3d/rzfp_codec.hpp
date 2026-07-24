#pragma once

#include "erwt3d/relative_error.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace erwt3d {

constexpr uint32_t RZFP_LEAF_DIM = 4;
constexpr uint32_t RZFP_LEAF_VALUES = 64;
constexpr uint32_t RZFP_LEAF_RAW_BYTES = 256;

enum class RzfpLeafCodec : uint8_t {
    RawFloat32 = 0,
    ConstantZero = 1,
    ConstantValue = 2,
    ZfpAccuracy = 3,
    ZfpAccuracyExceptions = 4,
    ZfpPrecision = 5,
};

enum class RzfpExceptionFill : uint8_t {
    Zero = 0,
    Mean = 1,
};

struct RzfpCodecConfig {
    RelativeErrorConfig error;

    std::vector<uint8_t> optional_exception_counts {
        0, 1, 2, 4, 8, 16
    };

    std::vector<uint8_t> precisions {
        12, 14, 16, 18, 20, 22, 24
    };

    std::vector<RzfpExceptionFill> fill_modes {
        RzfpExceptionFill::Zero,
        RzfpExceptionFill::Mean,
    };

    uint8_t max_total_exceptions = 24;

    double minimum_size_gain = 0.03;

    bool try_accuracy = true;
    bool try_accuracy_exceptions = true;
    bool try_precision = true;
    bool try_constant = true;
};
struct RzfpCandidate {

    RzfpLeafCodec codec = RzfpLeafCodec::RawFloat32;

    std::vector<uint8_t> payload;

    uint64_t exception_mask = 0;
    std::vector<float> exception_values;

    int16_t parameter = 0;

    RzfpExceptionFill fill_mode =
        RzfpExceptionFill::Zero;

    BlockErrorStats error_stats;

    uint32_t serialized_size = 0;
    uint32_t zfp_payload_size = 0;
    uint32_t exception_count = 0;

    double encode_ns = 0.0;
    double decode_ns = 0.0;

    bool passed = false;
};

struct RzfpCodecProfile {
    uint64_t raw_count = 0;
    uint64_t zero_count = 0;
    uint64_t constant_count = 0;
    uint64_t accuracy_count = 0;
    uint64_t accuracy_exception_count = 0;
    uint64_t precision_count = 0;

    uint64_t compressed_payload_bytes = 0;
    uint64_t decoded_value_count = 0;
    uint64_t scattered_value_count = 0;

    uint64_t payload_copy_ns = 0;
    uint64_t zfp_decompress_ns = 0;
    uint64_t exception_patch_ns = 0;
    uint64_t leaf_copy_ns = 0;
    uint64_t exception_alloc_ns = 0;
};

class RzfpCodec {
public:
    RzfpCodec();
    ~RzfpCodec();

    RzfpCodec(const RzfpCodec&) = delete;
    RzfpCodec& operator=(const RzfpCodec&) = delete;

    RzfpCandidate encodeBest(
        const float input[RZFP_LEAF_VALUES],
        uint64_t valid_mask,
        const RzfpCodecConfig& config
    );

    bool decode(
        const RzfpCandidate& encoded,
        float output[RZFP_LEAF_VALUES]
    );

    bool decodeRecord(
        RzfpLeafCodec codec,
        const uint8_t* data,
        size_t size,
        float output[RZFP_LEAF_VALUES],
        RzfpCodecProfile* profile = nullptr
    );

    const float* decodedData() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace erwt3d
