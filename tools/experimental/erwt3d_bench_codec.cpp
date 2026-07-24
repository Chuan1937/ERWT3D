#include "erwt3d/rzfp_codec.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

namespace {
double nowMs() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

int main() {
    const int kLeaves = 100000;

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);

    std::vector<float> raw(kLeaves * 64);
    for (auto& f : raw) f = dist(rng);

    erwt3d::RzfpCodec zc;
    erwt3d::RzfpCodecConfig cfg;
    cfg.error.contest_bound = 0.001;

    std::vector<erwt3d::RzfpCandidate> cands(kLeaves);
    double t0 = nowMs();
    for (int i = 0; i < kLeaves; ++i)
        cands[i] = zc.encodeBest(&raw[i*64], ~0u, cfg);
    double encMs = nowMs() - t0;

    uint64_t totalBytes = 0;
    for (auto& c : cands) totalBytes += c.serialized_size;

    std::vector<float> dec(kLeaves * 64);
    t0 = nowMs();
    for (int i = 0; i < kLeaves; ++i)
        zc.decode(cands[i], &dec[i*64]);
    double decMs = nowMs() - t0;

    double maxErr = 0; int viol = 0;
    for (size_t i = 0; i < raw.size(); ++i) {
        double a = std::abs((double)raw[i]);
        double e = a > 1e-30 ? std::abs(raw[i] - dec[i]) / a : std::abs(raw[i] - dec[i]);
        if (e > maxErr) maxErr = e;
        if (e > 0.001) ++viol;
    }

    double rawSec = encMs + decMs;
    double throughputGBs = (kLeaves * 64 * sizeof(float) / 1e9) / (rawSec / 1000);

    printf("=== ZFP Codec Benchmark ===\n");
    printf("Leaves: %d x 64 floats = %.1f MB\n", kLeaves, kLeaves * 64.0f * 4 / 1e6);
    printf("Encode: %.1f ms\n", encMs);
    printf("Decode: %.1f ms\n", decMs);
    printf("Throughput: %.1f GB/s (encode+decode)\n", throughputGBs);
    printf("Compressed: %.2f MB (%.3fx)\n", totalBytes/1e6, (double)totalBytes/(kLeaves*64*4));
    printf("Max rel err: %.6f, violations: %d/%.0f\n", maxErr, viol, (double)kLeaves*64);
    return 0;
}
