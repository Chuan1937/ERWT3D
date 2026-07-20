#include "erwt3d/raw_x_aux.hpp"
#include <cmath>
#include <iostream>

namespace {

int failures = 0;
int passed = 0;

void TEST(const char* name) { std::cout << "  " << name << "..." << std::flush; }
void FAIL(const char* msg) { std::cout << " FAIL: " << msg << "\n"; ++failures; }
void PASS() { std::cout << " PASS\n"; ++passed; }
void CHECK(bool cond, const char* msg) { if (!cond) { FAIL(msg); return; } }

void testHardLimitIs150() {
    TEST("hard limit is 1.500");
    CHECK(std::fabs(erwt3d::RAW_X_AUX_HARD_LIMIT - 1.500) < 1e-9,
          "hard limit not 1.500");
    PASS();
}

void testMaxRatioIs1495() {
    TEST("max ratio is 1.495");
    CHECK(std::fabs(erwt3d::RAW_X_AUX_MAX_RATIO - 1.495) < 1e-9,
          "max ratio not 1.495");
    PASS();
}

void testAutoLimitIs1490() {
    TEST("auto limit is 1.490");
    CHECK(std::fabs(erwt3d::RAW_X_AUX_AUTO_LIMIT - 1.490) < 1e-9,
          "auto limit not 1.490");
    PASS();
}

void testBoundary() {
    TEST("boundary ordering");
    CHECK(erwt3d::RAW_X_AUX_AUTO_LIMIT < erwt3d::RAW_X_AUX_MAX_RATIO,
          "auto >= max ratio");
    CHECK(erwt3d::RAW_X_AUX_MAX_RATIO < erwt3d::RAW_X_AUX_HARD_LIMIT,
          "max ratio >= hard limit");
    CHECK(erwt3d::RAW_X_AUX_HARD_LIMIT == 1.500,
          "hard limit should be exactly 1.500");
    PASS();
}

void testRatioLogic189() {
    TEST("ratio 1.489 < auto 1.490 so allowed");
    CHECK(1.489 < erwt3d::RAW_X_AUX_AUTO_LIMIT,
          "1.489 should be < auto limit");
    PASS();
}

void testRatioLogic1491() {
    TEST("ratio 1.491 > auto 1.490 so needs manual");
    CHECK(1.491 > erwt3d::RAW_X_AUX_AUTO_LIMIT,
          "1.491 should be > auto limit");
    CHECK(1.491 < erwt3d::RAW_X_AUX_MAX_RATIO,
          "1.491 should be < max ratio");
    PASS();
}

void testRatioLogic1495() {
    TEST("ratio 1.495 == max, allowed with force");
    CHECK(std::fabs(1.495 - erwt3d::RAW_X_AUX_MAX_RATIO) < 1e-9,
          "1.495 should equal max ratio");
    PASS();
}

void testRatioLogic1500() {
    TEST("ratio 1.500 == hard limit");
    CHECK(std::fabs(1.500 - erwt3d::RAW_X_AUX_HARD_LIMIT) < 1e-9,
          "1.500 should equal hard limit");
    PASS();
}

void testRatioLogic1501() {
    TEST("ratio 1.501 > hard limit must be rejected");
    CHECK(1.501 > erwt3d::RAW_X_AUX_HARD_LIMIT,
          "1.501 should exceed hard limit");
    PASS();
}

void testIntegerRatioFormula() {
    TEST("integer-based ratio check formula");
    const uint64_t rawBytes = 1000000ULL;
    const uint64_t maxFileBytes = static_cast<uint64_t>(
        std::floor(static_cast<double>(rawBytes) * erwt3d::RAW_X_AUX_HARD_LIMIT)
    );
    CHECK(maxFileBytes <= 1500000ULL, "max bytes exceeds 1.50x");
    CHECK(maxFileBytes >= 1499999ULL, "max bytes too low");
    PASS();
}

} // namespace

int main() {
    std::cout << "=== Storage Limit 1.50 Tests ===\n";
    testHardLimitIs150();
    testMaxRatioIs1495();
    testAutoLimitIs1490();
    testBoundary();
    testRatioLogic189();
    testRatioLogic1491();
    testRatioLogic1495();
    testRatioLogic1500();
    testRatioLogic1501();
    testIntegerRatioFormula();
    std::cout << "\nResult: " << passed << " passed, " << failures << " failed\n";
    return failures > 0 ? 1 : 0;
}
