#include "erwt3d/contest_phase_plan.hpp"
#include <iostream>
#include <vector>

namespace {

int failures = 0, passed = 0;
void TEST(const char* n) { std::cout << "  " << n << "..." << std::flush; }
void FAIL(const char* m) { std::cout << " FAIL: " << m << "\n"; ++failures; }
void PASS() { std::cout << " PASS\n"; ++passed; }
void CHECK(bool c, const char* m) { if (!c) { FAIL(m); return; } }

void testAllFitsOnePhase() {
    TEST("all groups fit in one phase");
    std::vector<uint64_t> bytes = {100, 200, 300, 50, 80, 70};
    std::vector<erwt3d::SliceAxis> axes = {
        erwt3d::SliceAxis::X, erwt3d::SliceAxis::Y, erwt3d::SliceAxis::Z,
        erwt3d::SliceAxis::X, erwt3d::SliceAxis::Y, erwt3d::SliceAxis::Z};
    std::vector<std::string> modes = {
        "random", "random", "random", "continuous", "continuous", "continuous"};

    auto plan = erwt3d::buildContestPhasePlan(bytes, axes, modes, 1000);
    CHECK(plan.phases.size() == 1, "wrong phase count");
    CHECK(plan.all_outputs_deferred, "should be all deferred");
    std::string err;
    CHECK(erwt3d::validateContestPhasePlan(plan, 6, 1000, &err), err.c_str());
    PASS();
}

void testNoDuplicateGroups() {
    TEST("no duplicate groups");
    std::vector<uint64_t> bytes = {300, 300, 300, 60, 60, 60};
    std::vector<erwt3d::SliceAxis> axes = {
        erwt3d::SliceAxis::X, erwt3d::SliceAxis::Y, erwt3d::SliceAxis::Z,
        erwt3d::SliceAxis::X, erwt3d::SliceAxis::Y, erwt3d::SliceAxis::Z};
    std::vector<std::string> modes = {
        "random", "random", "random", "continuous", "continuous", "continuous"};

    auto plan = erwt3d::buildContestPhasePlan(bytes, axes, modes, 500);
    std::string err;
    CHECK(erwt3d::validateContestPhasePlan(plan, 6, 500, &err), err.c_str());
    PASS();
}

void testLowMemorySplit() {
    TEST("low memory splits into phases");
    std::vector<uint64_t> bytes = {300, 300, 300, 60, 60, 60};
    std::vector<erwt3d::SliceAxis> axes = {
        erwt3d::SliceAxis::X, erwt3d::SliceAxis::Y, erwt3d::SliceAxis::Z,
        erwt3d::SliceAxis::X, erwt3d::SliceAxis::Y, erwt3d::SliceAxis::Z};
    std::vector<std::string> modes = {
        "random", "random", "random", "continuous", "continuous", "continuous"};

    auto plan = erwt3d::buildContestPhasePlan(bytes, axes, modes, 400);
    CHECK(plan.phases.size() >= 2, "should split into multiple phases");
    std::string err;
    CHECK(erwt3d::validateContestPhasePlan(plan, 6, 400, &err), err.c_str());
    PASS();
}

void testTinyBudget() {
    TEST("tiny budget");
    std::vector<uint64_t> bytes = {100, 120, 130, 50, 80, 70};
    std::vector<erwt3d::SliceAxis> axes = {
        erwt3d::SliceAxis::X, erwt3d::SliceAxis::Y, erwt3d::SliceAxis::Z,
        erwt3d::SliceAxis::X, erwt3d::SliceAxis::Y, erwt3d::SliceAxis::Z};
    std::vector<std::string> modes = {
        "random", "random", "random", "continuous", "continuous", "continuous"};

    auto plan = erwt3d::buildContestPhasePlan(bytes, axes, modes, 150);
    CHECK(plan.phases.size() >= 4, "tiny budget should produce many phases");
    std::string err;
    CHECK(erwt3d::validateContestPhasePlan(plan, 6, 150, &err), err.c_str());
    PASS();
}

void testRejectsUnknownGroup() {
    TEST("validate rejects unknown group id");
    erwt3d::ContestPhasePlan plan;
    plan.phases.push_back({{0, 5}, 100});
    std::string err;
    CHECK(!erwt3d::validateContestPhasePlan(plan, 3, 200, &err), "should fail");
    CHECK(!err.empty(), "no error msg");
    PASS();
}

void testRejectsOverBudget() {
    TEST("validate rejects over-budget phase");
    erwt3d::ContestPhasePlan plan;
    plan.phases.push_back({{0, 1}, 500});
    std::string err;
    CHECK(!erwt3d::validateContestPhasePlan(plan, 2, 200, &err), "should fail");
    PASS();
}

} // namespace

int main() {
    std::cout << "=== Contest Phase Plan Tests ===\n";
    testAllFitsOnePhase();
    testNoDuplicateGroups();
    testLowMemorySplit();
    testTinyBudget();
    testRejectsUnknownGroup();
    testRejectsOverBudget();
    std::cout << "\nResult: " << passed << " passed, " << failures << " failed\n";
    return failures > 0 ? 1 : 0;
}
