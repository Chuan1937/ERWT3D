#include "erwt3d/contest_positions.hpp"
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_set>
#include <unistd.h>

using namespace erwt3d;

static int testsPassed = 0;
static int testsFailed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " at line " << __LINE__ << "\n"; \
        ++testsFailed; \
    } else { ++testsPassed; } \
} while(0)

static void testParseCSV() {
    const char* path = "/tmp/test_positions.csv";
    {
        std::ofstream out(path);
        out << "axis,type,index\n";
        for (int i = 0; i < 100; ++i) out << "x,random," << i << "\n";
        for (int i = 0; i < 100; ++i) out << "y,random," << i << "\n";
        for (int i = 0; i < 100; ++i) out << "z,random," << i << "\n";
        for (int i = 500; i < 510; ++i) out << "x,continuous," << i << "\n";
        for (int i = 600; i < 610; ++i) out << "y,continuous," << i << "\n";
        for (int i = 700; i < 710; ++i) out << "z,continuous," << i << "\n";
    }

    ContestPositions pos;
    std::string err;
    CHECK(parsePositionsFile(path, 1000, 1000, 1000, 100, 10, pos, err), err);
    CHECK(pos.x_random.size() == 100, "x_random count");
    CHECK(pos.y_random.size() == 100, "y_random count");
    CHECK(pos.z_random.size() == 100, "z_random count");
    CHECK(pos.x_continuous.size() == 10, "x_continuous count");
    CHECK(pos.y_continuous.size() == 10, "y_continuous count");
    CHECK(pos.z_continuous.size() == 10, "z_continuous count");
    CHECK(pos.x_continuous[0] == 500, "x_continuous start");
    CHECK(pos.z_continuous[9] == 709, "z_continuous end");

    unlink(path);
}

static void testParseTXT() {
    const char* path = "/tmp/test_positions.txt";
    {
        std::ofstream out(path);
        for (int i = 0; i < 100; ++i) out << "x random " << i << "\n";
        for (int i = 0; i < 100; ++i) out << "y random " << i << "\n";
        for (int i = 0; i < 100; ++i) out << "z random " << i << "\n";
        for (int i = 200; i < 210; ++i) out << "x continuous " << i << "\n";
        for (int i = 300; i < 310; ++i) out << "y continuous " << i << "\n";
        for (int i = 400; i < 410; ++i) out << "z continuous " << i << "\n";
    }

    ContestPositions pos;
    std::string err;
    CHECK(parsePositionsFile(path, 1000, 1000, 1000, 100, 10, pos, err), err);
    CHECK(pos.x_random.size() == 100, "txt x_random");
    CHECK(pos.x_continuous[0] == 200, "txt x_continuous start");
    unlink(path);
}

static void testTooFewRandom() {
    const char* path = "/tmp/test_few.csv";
    {
        std::ofstream out(path);
        out << "axis,type,index\n";
        for (int i = 0; i < 50; ++i) out << "x,random," << i << "\n";
        for (int i = 0; i < 100; ++i) out << "y,random," << i << "\n";
        for (int i = 0; i < 100; ++i) out << "z,random," << i << "\n";
        for (int i = 500; i < 510; ++i) out << "x,continuous," << i << "\n";
        for (int i = 600; i < 610; ++i) out << "y,continuous," << i << "\n";
        for (int i = 700; i < 710; ++i) out << "z,continuous," << i << "\n";
    }

    ContestPositions pos;
    std::string err;
    CHECK(!parsePositionsFile(path, 1000, 1000, 1000, 100, 10, pos, err), "should fail: too few");
    CHECK(err.find("100") != std::string::npos, "error mentions count");
    unlink(path);
}

static void testDuplicateRandom() {
    const char* path = "/tmp/test_dup.csv";
    {
        std::ofstream out(path);
        out << "axis,type,index\n";
        for (int i = 0; i < 99; ++i) out << "x,random," << i << "\n";
        out << "x,random,0\n"; // duplicate
        for (int i = 0; i < 100; ++i) out << "y,random," << i << "\n";
        for (int i = 0; i < 100; ++i) out << "z,random," << i << "\n";
        for (int i = 500; i < 510; ++i) out << "x,continuous," << i << "\n";
        for (int i = 600; i < 610; ++i) out << "y,continuous," << i << "\n";
        for (int i = 700; i < 710; ++i) out << "z,continuous," << i << "\n";
    }

    ContestPositions pos;
    std::string err;
    CHECK(!parsePositionsFile(path, 1000, 1000, 1000, 100, 10, pos, err), "should fail: duplicate");
    CHECK(err.find("duplicate") != std::string::npos, "error mentions duplicate");
    unlink(path);
}

static void testOutOfBounds() {
    const char* path = "/tmp/test_oob.csv";
    {
        std::ofstream out(path);
        out << "axis,type,index\n";
        for (int i = 0; i < 100; ++i) out << "x,random," << i << "\n";
        for (int i = 0; i < 100; ++i) out << "y,random," << i << "\n";
        for (int i = 0; i < 99; ++i) out << "z,random," << i << "\n";
        out << "z,random,99999\n"; // out of bounds
        for (int i = 500; i < 510; ++i) out << "x,continuous," << i << "\n";
        for (int i = 600; i < 610; ++i) out << "y,continuous," << i << "\n";
        for (int i = 700; i < 710; ++i) out << "z,continuous," << i << "\n";
    }

    ContestPositions pos;
    std::string err;
    CHECK(!parsePositionsFile(path, 1000, 1000, 1000, 100, 10, pos, err), "should fail: oob");
    CHECK(err.find("99999") != std::string::npos || err.find(">=") != std::string::npos, "error mentions index or bounds");
    unlink(path);
}

static void testContinuousCount() {
    const char* path = "/tmp/test_ccont.csv";
    {
        std::ofstream out(path);
        out << "axis,type,index\n";
        for (int i = 0; i < 100; ++i) out << "x,random," << i << "\n";
        for (int i = 0; i < 100; ++i) out << "y,random," << i << "\n";
        for (int i = 0; i < 100; ++i) out << "z,random," << i << "\n";
        for (int i = 500; i < 508; ++i) out << "x,continuous," << i << "\n"; // only 8
        for (int i = 600; i < 610; ++i) out << "y,continuous," << i << "\n";
        for (int i = 700; i < 710; ++i) out << "z,continuous," << i << "\n";
    }

    ContestPositions pos;
    std::string err;
    CHECK(!parsePositionsFile(path, 1000, 1000, 1000, 100, 10, pos, err), "should fail: wrong cont count");
    unlink(path);
}

static void testContinuousNotSequential() {
    const char* path = "/tmp/test_cseq.csv";
    {
        std::ofstream out(path);
        out << "axis,type,index\n";
        for (int i = 0; i < 100; ++i) out << "x,random," << i << "\n";
        for (int i = 0; i < 100; ++i) out << "y,random," << i << "\n";
        for (int i = 0; i < 100; ++i) out << "z,random," << i << "\n";
        for (int i = 500; i < 509; ++i) out << "x,continuous," << i << "\n";
        out << "x,continuous,600\n"; // not sequential
        for (int i = 600; i < 610; ++i) out << "y,continuous," << i << "\n";
        for (int i = 700; i < 710; ++i) out << "z,continuous," << i << "\n";
    }

    ContestPositions pos;
    std::string err;
    CHECK(!parsePositionsFile(path, 1000, 1000, 1000, 100, 10, pos, err), "should fail: not sequential");
    CHECK(err.find("sequential") != std::string::npos, "error mentions sequential");
    unlink(path);
}

static void testOrderPreserved() {
    const char* path = "/tmp/test_order.csv";
    {
        std::ofstream out(path);
        out << "axis,type,index\n";
        std::vector<int> order = {5, 3, 7, 1, 9, 0, 2, 4, 6, 8};
        for (int i = 0; i < 100; ++i) {
            int v = i < 10 ? order[i] : i;
            out << "x,random," << v << "\n";
        }
        for (int i = 0; i < 100; ++i) out << "y,random," << i << "\n";
        for (int i = 0; i < 100; ++i) out << "z,random," << i << "\n";
        for (int i = 500; i < 510; ++i) out << "x,continuous," << i << "\n";
        for (int i = 600; i < 610; ++i) out << "y,continuous," << i << "\n";
        for (int i = 700; i < 710; ++i) out << "z,continuous," << i << "\n";
    }

    ContestPositions pos;
    std::string err;
    CHECK(parsePositionsFile(path, 1000, 1000, 1000, 100, 10, pos, err), err);
    CHECK(pos.x_random[0] == 5, "order preserved [0]");
    CHECK(pos.x_random[1] == 3, "order preserved [1]");
    CHECK(pos.x_random[2] == 7, "order preserved [2]");
    unlink(path);
}

static void testGenerateRandom() {
    ContestPositions pos;
    std::string err;
    CHECK(generateRandomPositions(1000, 1000, 1000, 100, 10, 42, pos, err), err);
    CHECK(pos.x_random.size() == 100, "gen x count");
    CHECK(pos.y_random.size() == 100, "gen y count");
    CHECK(pos.z_random.size() == 100, "gen z count");
    CHECK(pos.x_continuous.size() == 10, "gen xc count");
    CHECK(pos.y_continuous.size() == 10, "gen yc count");
    CHECK(pos.z_continuous.size() == 10, "gen zc count");

    std::unordered_set<uint64_t> seen;
    for (auto x : pos.x_random) {
        CHECK(seen.insert(x).second, "no duplicates in generated");
    }
}

static void testHashDeterminism() {
    ContestPositions pos1, pos2;
    std::string err;
    generateRandomPositions(1000, 1000, 1000, 100, 10, 42, pos1, err);
    generateRandomPositions(1000, 1000, 1000, 100, 10, 42, pos2, err);
    CHECK(computePositionsHash(pos1) == computePositionsHash(pos2), "hash deterministic");
}

int main() {
    testParseCSV();
    testParseTXT();
    testTooFewRandom();
    testDuplicateRandom();
    testOutOfBounds();
    testContinuousCount();
    testContinuousNotSequential();
    testOrderPreserved();
    testGenerateRandom();
    testHashDeterminism();

    std::cout << "Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed > 0 ? 1 : 0;
}
