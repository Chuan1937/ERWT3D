#include "erwt3d/rzfp_codec.hpp"
#include "erwt3d/relative_error.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

using namespace erwt3d;

static int fail_count = 0;

static void check(bool cond, const char* name) {
    if (!cond) {
        std::cerr << "FAIL: " << name << std::endl;
        ++fail_count;
    }
}

static RelativeErrorConfig makeStrictConfig() {
    RelativeErrorConfig cfg;
    cfg.policy = RelativeErrorPolicy::Strict;
    cfg.contest_bound = 1e-3;
    cfg.internal_bound = 7.5e-4;
    return cfg;
}

static bool validateRoundtrip(
    const float input[64],
    uint64_t valid_mask,
    const RzfpCandidate& cand,
    const RelativeErrorConfig& cfg
) {
    RzfpCodec codec;
    float output[64];
    if (!codec.decode(cand, output)) return false;
    auto stats = checkBlockError(input, output, 64, valid_mask, cfg);
    return stats.passed;
}

static void testZeroBlock() {
    float input[64] = {};
    RzfpCodecConfig cfg;
    cfg.error = makeStrictConfig();
    RzfpCodec codec;
    auto cand = codec.encodeBest(input, 0xFFFFFFFFFFFFFFFFULL, cfg);
    check(cand.passed, "zero block passed");
    check(cand.codec == RzfpLeafCodec::ConstantZero, "zero block uses ConstantZero");
    check(cand.serialized_size == 2, "zero block size == 2");
    check(validateRoundtrip(input, 0xFFFFFFFFFFFFFFFFULL, cand, cfg.error),
          "zero block roundtrip");
}

static void testConstantBlock() {
    float input[64];
    std::fill(input, input + 64, 3.14159f);
    RzfpCodecConfig cfg;
    cfg.error = makeStrictConfig();
    RzfpCodec codec;
    auto cand = codec.encodeBest(input, 0xFFFFFFFFFFFFFFFFULL, cfg);
    check(cand.passed, "constant block passed");
    check(cand.codec == RzfpLeafCodec::ConstantValue, "constant block uses ConstantValue");
    check(cand.serialized_size == 2 + sizeof(float), "constant block size");
    check(validateRoundtrip(input, 0xFFFFFFFFFFFFFFFFULL, cand, cfg.error),
          "constant block roundtrip");
}

static void testPositiveRamp() {
    float input[64];
    for (uint32_t i = 0; i < 64; ++i) {
        input[i] = static_cast<float>(i) * 0.1f + 1.0f;
    }
    RzfpCodecConfig cfg;
    cfg.error = makeStrictConfig();
    RzfpCodec codec;
    auto cand = codec.encodeBest(input, 0xFFFFFFFFFFFFFFFFULL, cfg);
    check(cand.passed, "positive ramp passed");
    check(cand.serialized_size <= 2 + 256, "positive ramp size bounded");
    check(validateRoundtrip(input, 0xFFFFFFFFFFFFFFFFULL, cand, cfg.error),
          "positive ramp roundtrip");
}

static void testSignedValues() {
    float input[64];
    for (uint32_t i = 0; i < 64; ++i) {
        input[i] = (i % 2 == 0 ? 1.0f : -1.0f) * (static_cast<float>(i) + 0.5f);
    }
    RzfpCodecConfig cfg;
    cfg.error = makeStrictConfig();
    RzfpCodec codec;
    auto cand = codec.encodeBest(input, 0xFFFFFFFFFFFFFFFFULL, cfg);
    check(cand.passed, "signed values passed");
    check(validateRoundtrip(input, 0xFFFFFFFFFFFFFFFFULL, cand, cfg.error),
          "signed values roundtrip");
}

static void testNearZeroValues() {
    float input[64];
    for (uint32_t i = 0; i < 64; ++i) {
        input[i] = 1e-5f * static_cast<float>(i + 1);
    }
    RzfpCodecConfig cfg;
    cfg.error = makeStrictConfig();
    RzfpCodec codec;
    auto cand = codec.encodeBest(input, 0xFFFFFFFFFFFFFFFFULL, cfg);
    check(cand.passed, "near-zero values passed");
    check(validateRoundtrip(input, 0xFFFFFFFFFFFFFFFFULL, cand, cfg.error),
          "near-zero values roundtrip");
}

static void testHighDynamicRange() {
    float input[64];
    for (uint32_t i = 0; i < 64; ++i) {
        input[i] = std::ldexp(1.0f, static_cast<int>(i % 20));
    }
    RzfpCodecConfig cfg;
    cfg.error = makeStrictConfig();
    RzfpCodec codec;
    auto cand = codec.encodeBest(input, 0xFFFFFFFFFFFFFFFFULL, cfg);
    check(cand.passed, "high dynamic range passed");
    check(validateRoundtrip(input, 0xFFFFFFFFFFFFFFFFULL, cand, cfg.error),
          "high dynamic range roundtrip");
}

static void testSubnormalBlock() {
    float input[64];
    float sub = std::numeric_limits<float>::denorm_min();
    std::fill(input, input + 64, sub);
    RzfpCodecConfig cfg;
    cfg.error = makeStrictConfig();
    RzfpCodec codec;
    auto cand = codec.encodeBest(input, 0xFFFFFFFFFFFFFFFFULL, cfg);
    check(cand.passed, "subnormal block passed");
    check(cand.codec == RzfpLeafCodec::RawFloat32, "subnormal falls back to raw");
    check(validateRoundtrip(input, 0xFFFFFFFFFFFFFFFFULL, cand, cfg.error),
          "subnormal block roundtrip");
}

static void testNanInfBlock() {
    float input[64];
    std::fill(input, input + 32, std::numeric_limits<float>::quiet_NaN());
    std::fill(input + 32, input + 64, std::numeric_limits<float>::infinity());
    RzfpCodecConfig cfg;
    cfg.error = makeStrictConfig();
    RzfpCodec codec;
    auto cand = codec.encodeBest(input, 0xFFFFFFFFFFFFFFFFULL, cfg);
    check(cand.passed, "NaN/Inf block passed");
    check(cand.codec == RzfpLeafCodec::RawFloat32, "NaN/Inf falls back to raw");
    check(validateRoundtrip(input, 0xFFFFFFFFFFFFFFFFULL, cand, cfg.error),
          "NaN/Inf block roundtrip");
}

static void testBoundaryLeaf() {
    float input[64];
    std::mt19937 gen(12345);
    std::uniform_real_distribution<float> dist(-10.0f, 10.0f);
    for (uint32_t i = 0; i < 64; ++i) input[i] = dist(gen);

    uint64_t valid_mask = 0;
    for (uint32_t z = 0; z < 2; ++z)
        for (uint32_t y = 0; y < 2; ++y)
            for (uint32_t x = 0; x < 2; ++x)
                valid_mask |= uint64_t{1} << ((z * 4 + y) * 4 + x);

    RzfpCodecConfig cfg;
    cfg.error = makeStrictConfig();
    RzfpCodec codec;
    auto cand = codec.encodeBest(input, valid_mask, cfg);
    if (!cand.passed) {
        std::cerr << "boundary leaf not passed, codec=" << static_cast<int>(cand.codec)
                  << " size=" << cand.serialized_size << " viol=" << cand.error_stats.violation_count
                  << " max_rel=" << cand.error_stats.max_relative_error << std::endl;
    }
    check(cand.passed, "boundary leaf passed");
    check(validateRoundtrip(input, valid_mask, cand, cfg.error),
          "boundary leaf roundtrip");
}

static void testRawFallback() {
    float input[64];
    std::fill(input, input + 64, 1.0f);
    input[31] = 0.0f;
    RzfpCodecConfig cfg;
    cfg.error = makeStrictConfig();
    cfg.minimum_size_gain = 0.99;
    RzfpCodec codec;
    auto cand = codec.encodeBest(input, 0xFFFFFFFFFFFFFFFFULL, cfg);
    check(cand.passed, "raw fallback passed");
    check(cand.codec == RzfpLeafCodec::RawFloat32, "raw fallback selected");
    check(validateRoundtrip(input, 0xFFFFFFFFFFFFFFFFULL, cand, cfg.error),
          "raw fallback roundtrip");
}

static void testTruncatedRecord() {
    RzfpCodec codec;
    float input[64];
    std::fill(input, input + 64, 1.0f);
    RzfpCodecConfig cfg;
    cfg.error = makeStrictConfig();
    auto cand = codec.encodeBest(input, 0xFFFFFFFFFFFFFFFFULL, cfg);
    check(cand.passed, "truncated base passed");

    RzfpCandidate truncated = cand;
    if (truncated.payload.size() > 1) {
        truncated.payload.resize(truncated.payload.size() / 2);
        float output[64];
        bool ok = codec.decode(truncated, output);
        check(!ok, "truncated record decode fails");
    }
}

static void testManyRandomBlocks() {
    std::mt19937 gen(20260511);
    std::uniform_real_distribution<float> dist(-100.0f, 100.0f);
    RzfpCodecConfig cfg;
    cfg.error = makeStrictConfig();

    for (int b = 0; b < 100; ++b) {
        float input[64];
        for (uint32_t i = 0; i < 64; ++i) input[i] = dist(gen);
        RzfpCodec codec;
        auto cand = codec.encodeBest(input, 0xFFFFFFFFFFFFFFFFULL, cfg);
        if (!cand.passed) {
            std::cerr << "FAIL: random block " << b << " did not pass" << std::endl;
            ++fail_count;
            continue;
        }
        if (!validateRoundtrip(input, 0xFFFFFFFFFFFFFFFFULL, cand, cfg.error)) {
            std::cerr << "FAIL: random block " << b << " roundtrip violation" << std::endl;
            ++fail_count;
        }
    }
}

int main() {
    testZeroBlock();
    testConstantBlock();
    testPositiveRamp();
    testSignedValues();
    testNearZeroValues();
    testHighDynamicRange();
    testSubnormalBlock();
    testNanInfBlock();
    testBoundaryLeaf();
    testRawFallback();
    testTruncatedRecord();
    testManyRandomBlocks();

    if (fail_count > 0) {
        std::cerr << "Total failures: " << fail_count << std::endl;
        return 1;
    }
    std::cout << "All RZFP codec tests passed" << std::endl;
    return 0;
}
