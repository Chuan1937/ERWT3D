#include "erwt3d/memory_budget.hpp"
#include "erwt3d/window_cache.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << "FAIL: " << #condition \
                  << " at line " << __LINE__ << "\n"; \
        ++failures; \
    } \
} while (false)

constexpr uint64_t MiB = 1024ULL * 1024ULL;
constexpr uint64_t GiB = 1024ULL * MiB;

void testExplicitMemoryBudgets() {
    const uint64_t payload = 21ULL * GiB;
    const uint64_t outputSlice = 24ULL * MiB;

    const auto twoGiB = erwt3d::makeMemoryBudget(
        "2048",
        payload,
        outputSlice,
        100
    );
    CHECK(twoGiB.valid);
    CHECK(!twoGiB.automatic);
    CHECK(twoGiB.total_bytes == 2ULL * GiB);
    CHECK(twoGiB.accountedBytes() <= twoGiB.total_bytes);
    CHECK(twoGiB.output_batch_size >= 1);
    CHECK(twoGiB.output_batch_size <= 100);

    const auto eightGiB = erwt3d::makeMemoryBudget(
        "8192",
        payload,
        outputSlice,
        100
    );
    CHECK(eightGiB.valid);
    CHECK(eightGiB.total_bytes == 8ULL * GiB);
    CHECK(eightGiB.accountedBytes() <= eightGiB.total_bytes);
    CHECK(eightGiB.output_batch_size >= twoGiB.output_batch_size);
    CHECK(eightGiB.window_cache_bytes >= twoGiB.window_cache_bytes);

    const auto tooSmall = erwt3d::makeMemoryBudget(
        "128",
        payload,
        outputSlice,
        100
    );
    CHECK(!tooSmall.valid);

    const auto malformed = erwt3d::makeMemoryBudget(
        "2GB",
        payload,
        outputSlice,
        100
    );
    CHECK(!malformed.valid);
}

void testAutomaticMemoryBudget() {
    const uint64_t available = erwt3d::readLinuxMemAvailableBytes();
    if (available == 0) {
        std::cout << "SKIP: /proc/meminfo MemAvailable unavailable\n";
        return;
    }

    const auto budget = erwt3d::makeMemoryBudget(
        "auto",
        21ULL * GiB,
        24ULL * MiB,
        100
    );

    CHECK(budget.valid);
    CHECK(budget.automatic);
    CHECK(budget.total_bytes <= 32ULL * GiB);
    CHECK(budget.total_bytes <= available / 2);
    CHECK(budget.accountedBytes() <= budget.total_bytes);
    CHECK(budget.output_batch_size >= 1);
}

std::vector<uint8_t> bytes(uint8_t value, size_t count) {
    return std::vector<uint8_t>(count, value);
}

void testWindowCacheLru() {
    erwt3d::BoundedWindowCache cache(8);

    const erwt3d::WindowCacheKey keyA{1, 0, 4};
    const erwt3d::WindowCacheKey keyB{1, 4, 4};
    const erwt3d::WindowCacheKey keyC{1, 8, 4};

    CHECK(cache.put(keyA, bytes(1, 4)));
    CHECK(cache.put(keyB, bytes(2, 4)));
    CHECK(cache.residentBytes() == 8);

    std::shared_ptr<const std::vector<uint8_t>> result;
    CHECK(cache.get(keyA, result));
    CHECK(result && result->size() == 4 && (*result)[0] == 1);

    // A was just touched, so B is the least-recently-used entry.
    CHECK(cache.put(keyC, bytes(3, 4)));
    CHECK(cache.residentBytes() <= cache.capacityBytes());
    CHECK(cache.evictionCount() == 1);

    CHECK(!cache.get(keyB, result));
    CHECK(cache.get(keyA, result));
    CHECK(cache.get(keyC, result));
    CHECK(cache.hitCount() == 3);
    CHECK(cache.missCount() == 1);
}

void testWindowCacheLimitsAndClear() {
    erwt3d::BoundedWindowCache cache(4);
    const erwt3d::WindowCacheKey large{2, 0, 8};
    CHECK(!cache.put(large, bytes(9, 8)));
    CHECK(cache.residentBytes() == 0);

    const erwt3d::WindowCacheKey small{2, 0, 4};
    CHECK(cache.put(small, bytes(7, 4)));
    CHECK(cache.residentBytes() == 4);

    cache.setCapacityBytes(2);
    CHECK(cache.residentBytes() == 0);
    CHECK(cache.evictionCount() == 1);

    cache.clear();
    CHECK(cache.residentBytes() == 0);
    CHECK(cache.hitCount() == 0);
    CHECK(cache.missCount() == 0);
    CHECK(cache.evictionCount() == 0);
}

} // namespace

int main() {
    std::cout << "test_memory_cache\n";

    testExplicitMemoryBudgets();
    testAutomaticMemoryBudget();
    testWindowCacheLru();
    testWindowCacheLimitsAndClear();

    if (failures == 0) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    }

    std::cerr << failures << " TESTS FAILED\n";
    return 1;
}
