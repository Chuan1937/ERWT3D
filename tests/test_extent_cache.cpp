#include "erwt3d/window_cache.hpp"
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

namespace {

int failures = 0;
int passed = 0;

void TEST(const char* name) { std::cout << "  " << name << "..." << std::flush; }
void FAIL(const char* msg) { std::cout << " FAIL: " << msg << "\n"; ++failures; }
void PASS() { std::cout << " PASS\n"; ++passed; }
void CHECK(bool cond, const char* msg) { if (!cond) { FAIL(msg); return; } }

void testExactHit() {
    TEST("exact hit");
    erwt3d::BoundedWindowCache cache(1024 * 1024);

    auto data = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>(256, 0xAB)
    );
    erwt3d::WindowCacheKey key{1, 100, 256};
    cache.putShared(key, data);

    std::shared_ptr<const std::vector<uint8_t>> result;
    CHECK(cache.get(key, result), "exact get failed");
    CHECK(result && result->size() == 256, "wrong size");
    CHECK((*result)[0] == 0xAB, "wrong data");
    PASS();
}

void testContainedHit() {
    TEST("contained hit");
    erwt3d::BoundedWindowCache cache(1024 * 1024);

    auto data = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>(512, 0xCD)
    );
    cache.putShared({1, 0, 512}, data);

    std::shared_ptr<const std::vector<uint8_t>> result;
    uint64_t cachedOffset = 0;
    CHECK(cache.getContaining(1, 100, 64, result, &cachedOffset),
          "contained get failed");
    CHECK(cachedOffset == 0, "wrong cached offset");
    CHECK(result && result->size() == 512, "wrong cached size");
    PASS();
}

void testContainedExactEdge() {
    TEST("contained exact edge");
    erwt3d::BoundedWindowCache cache(1024 * 1024);

    auto data = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>(512, 0xEF)
    );
    cache.putShared({1, 0, 512}, data);

    std::shared_ptr<const std::vector<uint8_t>> result;
    CHECK(cache.getContaining(1, 0, 512, result),
          "contained same-size failed");
    CHECK(result && result->size() == 512, "wrong size");
    PASS();
}

void testLeftOverhangMiss() {
    TEST("left overhang miss");
    erwt3d::BoundedWindowCache cache(1024 * 1024);

    auto data = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>(512, 0x11)
    );
    cache.putShared({1, 100, 512}, data);

    std::shared_ptr<const std::vector<uint8_t>> result;
    CHECK(!cache.getContaining(1, 50, 600, result),
          "left overhang should miss");
    PASS();
}

void testRightOverhangMiss() {
    TEST("right overhang miss");
    erwt3d::BoundedWindowCache cache(1024 * 1024);

    auto data = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>(512, 0x22)
    );
    cache.putShared({1, 100, 512}, data);

    std::shared_ptr<const std::vector<uint8_t>> result;
    CHECK(!cache.getContaining(1, 200, 500, result),
          "right overhang should miss");
    PASS();
}

void testDifferentFileMiss() {
    TEST("different file identity miss");
    erwt3d::BoundedWindowCache cache(1024 * 1024);

    auto data = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>(512, 0x33)
    );
    cache.putShared({1, 0, 512}, data);

    std::shared_ptr<const std::vector<uint8_t>> result;
    CHECK(!cache.getContaining(2, 100, 64, result),
          "different file should miss");
    PASS();
}

void testLruEviction() {
    TEST("LRU eviction");
    erwt3d::BoundedWindowCache cache(2048);

    for (int i = 0; i < 20; ++i) {
        auto data = std::make_shared<const std::vector<uint8_t>>(
            std::vector<uint8_t>(256, static_cast<uint8_t>(i))
        );
        cache.putShared({1, static_cast<uint64_t>(i) * 256, 256}, data);
    }

    CHECK(cache.residentBytes() <= 2048,
          "resident bytes exceed capacity");
    CHECK(cache.evictionCount() > 0, "no evictions");
    PASS();
}

void testClear() {
    TEST("clear resets all stats");
    erwt3d::BoundedWindowCache cache(1024 * 1024);

    auto data = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>(256, 0x44)
    );
    cache.putShared({1, 0, 256}, data);
    cache.clear();

    CHECK(cache.residentBytes() == 0, "resident not zero after clear");
    CHECK(cache.hitCount() == 0, "hit count not zero");
    std::shared_ptr<const std::vector<uint8_t>> result;
    CHECK(!cache.get({1, 0, 256}, result), "should miss after clear");
    PASS();
}

void testTooLargeRejected() {
    TEST("entry larger than capacity rejected");
    erwt3d::BoundedWindowCache cache(512);
    CHECK(!cache.put({1, 0, 1024}, std::vector<uint8_t>(1024, 0)),
          "should reject oversized entry");
    CHECK(cache.residentBytes() == 0, "resident should be zero");
    PASS();
}

void testStatistics() {
    TEST("hit/miss/contained statistics");
    erwt3d::BoundedWindowCache cache(1024 * 1024);

    auto data = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>(512, 0x55)
    );
    cache.putShared({1, 0, 512}, data);

    std::shared_ptr<const std::vector<uint8_t>> result;
    cache.get({1, 0, 512}, result);
    cache.get({99, 0, 512}, result);
    cache.getContaining(1, 0, 256, result);

    CHECK(cache.hitCount() == 1, "exact hit count wrong");
    CHECK(cache.missCount() == 1, "miss count wrong");
    CHECK(cache.containedHitCount() == 1, "contained hit count wrong");
    CHECK(cache.savedReadBytes() == 256, "saved bytes wrong");
    PASS();
}

} // namespace

int main() {
    std::cout << "=== Extent Cache Tests ===\n";
    testExactHit();
    testContainedHit();
    testContainedExactEdge();
    testLeftOverhangMiss();
    testRightOverhangMiss();
    testDifferentFileMiss();
    testLruEviction();
    testClear();
    testTooLargeRejected();
    testStatistics();
    std::cout << "\nResult: " << passed << " passed, " << failures << " failed\n";
    return failures > 0 ? 1 : 0;
}
