#include "erwt3d/relative_error.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

using namespace erwt3d;

static int fail_count = 0;

static void check(bool cond, const char* name) {
    if (!cond) {
        std::cerr << "FAIL: " << name << std::endl;
        ++fail_count;
    }
}

static void runStrictTests() {
    RelativeErrorConfig cfg;
    cfg.policy = RelativeErrorPolicy::Strict;
    cfg.contest_bound = 1e-3;

    check(checkPointwiseError(1.0f, 1.0f, cfg).passed, "exact positive");
    check(checkPointwiseError(-2.0f, -2.0f, cfg).passed, "exact negative");

    {
        auto r = checkPointwiseError(1.0f, 1.00099f, cfg);
        check(r.passed && r.relative_error < 1e-3, "rel just below 0.001");
    }
    {
        auto r = checkPointwiseError(1000.0f, 1001.0f, cfg);
        check(!r.passed && std::abs(r.relative_error - 1e-3) < 1e-12,
              "rel exactly 0.001 must fail");
    }
    {
        auto r = checkPointwiseError(1.0f, 1.002f, cfg);
        check(!r.passed && r.relative_error > 1e-3, "rel above 0.001 fails");
    }

    check(checkPointwiseError(0.0f, 0.0f, cfg).passed, "zero exact");
    check(!checkPointwiseError(0.0f, 1e-12f, cfg).passed, "zero nonzero fails");
    check(checkPointwiseError(-0.0f, 0.0f, cfg).passed, "negative zero to positive zero");

    {
        float sub = std::numeric_limits<float>::denorm_min();
        auto r = checkPointwiseError(sub, sub, cfg);
        check(r.passed, "subnormal exact");
    }

    {
        float nan = std::numeric_limits<float>::quiet_NaN();
        check(checkPointwiseError(nan, nan, cfg).passed, "NaN to NaN");
        check(!checkPointwiseError(nan, 0.0f, cfg).passed, "NaN to number fails");
    }

    {
        float inf = std::numeric_limits<float>::infinity();
        check(checkPointwiseError(inf, inf, cfg).passed, "+Inf to +Inf");
        check(checkPointwiseError(-inf, -inf, cfg).passed, "-Inf to -Inf");
        check(!checkPointwiseError(inf, -inf, cfg).passed, "+Inf to -Inf fails");
        check(!checkPointwiseError(inf, 1e30f, cfg).passed, "Inf to finite fails");
    }

    {
        float orig[4] = {1.0f, 2.0f, 0.0f, 4.0f};
        float recon[4] = {1.0f, 2.0f, 0.0f, 4.0f};
        auto stats = checkBlockError(orig, recon, 4, 0xFF, cfg);
        check(stats.passed && stats.valid_count == 4 && stats.violation_count == 0,
              "block exact");
    }
    {
        float orig[4] = {1.0f, 2.0f, 3.0f, 4.0f};
        float recon[4] = {1.0f, 2.0f, 3.003f, 4.0f};
        auto stats = checkBlockError(orig, recon, 4, 0x0F, cfg);
        check(!stats.passed && stats.violation_count == 1,
              "block one violation");
    }
    {
        float orig[4] = {1.0f, 2.0f, 3.0f, 4.0f};
        float recon[4] = {1.0f, 2.0f, 3.0f, 4.0f};
        auto stats = checkBlockError(orig, recon, 4, 0x03, cfg);
        check(stats.passed && stats.valid_count == 2,
              "block valid mask respected");
    }
}

static void runLegacyTests() {
    RelativeErrorConfig cfg;
    cfg.policy = RelativeErrorPolicy::Legacy;
    cfg.contest_bound = 1e-3;
    cfg.legacy_zero_abs_tol = 1e-6;

    check(checkPointwiseError(1.0f, 1.0f, cfg).passed, "legacy exact");
    check(!checkPointwiseError(1.0f, 1.002f, cfg).passed, "legacy rel fail");

    check(checkPointwiseError(1e-7f, 1.5e-7f, cfg).passed,
          "legacy near-zero within abs tol");
    check(!checkPointwiseError(1e-7f, 2e-6f, cfg).passed,
          "legacy near-zero outside abs tol");

    check(checkPointwiseError(0.0f, 1e-7f, cfg).passed,
          "legacy zero within abs tol");
    check(!checkPointwiseError(0.0f, 2e-6f, cfg).passed,
          "legacy zero outside abs tol");
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        runStrictTests();
        runLegacyTests();
    } else {
        std::string mode(argv[1]);
        if (mode == "strict") {
            runStrictTests();
        } else if (mode == "legacy") {
            runLegacyTests();
        } else {
            std::cerr << "Unknown mode: " << mode << std::endl;
            return 2;
        }
    }

    if (fail_count > 0) {
        std::cerr << "Total failures: " << fail_count << std::endl;
        return 1;
    }
    std::cout << "All relative_error tests passed" << std::endl;
    return 0;
}
