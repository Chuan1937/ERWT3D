#include "erwt3d/contest_positions.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <unordered_set>

namespace erwt3d {

namespace {

static std::string axisName(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = static_cast<char>(std::tolower(c));
    return r;
}

static ParseLineResult parseLine(const std::string& line, int lineNum,
                                  std::string& axis, std::string& type, uint64_t& index,
                                  std::string& error) {
    std::string l = line;
    while (!l.empty() && (l.back() == '\r' || l.back() == '\n' || l.back() == ' '))
        l.pop_back();
    while (!l.empty() && (l.front() == ' ' || l.front() == '\t'))
        l.erase(l.begin());
    if (l.empty() || l[0] == '#') return ParseLineResult::Skip;

    if (l.size() >= 3 && static_cast<uint8_t>(l[0]) == 0xEF &&
        static_cast<uint8_t>(l[1]) == 0xBB &&
        static_cast<uint8_t>(l[2]) == 0xBF) {
        l = l.substr(3);
    }

    for (auto& c : l) if (c == ',' || c == '\t') c = ' ';

    {
        std::string lower;
        for (char c : l) {
            if (c != ' ') lower.push_back(static_cast<char>(std::tolower(c)));
        }
        std::string lowerWithSpaces = l;
        for (auto& c : lowerWithSpaces) c = static_cast<char>(std::tolower(c));

        if (lower == "axistypeindex" ||
            lowerWithSpaces == "axis type index") {
            return ParseLineResult::Skip;
        }
    }

    std::istringstream iss(l);
    std::string a, t;
    uint64_t idx;
    if (!(iss >> a >> t >> idx)) {
        error = "positions.csv line " + std::to_string(lineNum) +
                ": expected \"axis,type,index\", got \"" + l + "\"";
        return ParseLineResult::Error;
    }

    axis = axisName(a);
    for (auto& c : t) c = static_cast<char>(std::tolower(c));
    type = t;
    index = idx;

    if (axis == "axis" && type == "type" && index == 0) {
        return ParseLineResult::Skip;
    }

    std::string extra;
    if (iss >> extra) {
        error = "positions.csv line " + std::to_string(lineNum) +
                ": expected \"axis,type,index\", got \"" + l + "\"";
        return ParseLineResult::Error;
    }

    return ParseLineResult::Parsed;
}

static void hashByte(uint64_t& h, uint8_t value) {
    h ^= value;
    h *= 1099511628211ULL;
}

static void hashU64(uint64_t& h, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        hashByte(h, static_cast<uint8_t>((value >> (i * 8)) & 0xff));
    }
}

static void hashString(uint64_t& h, const std::string& s) {
    for (char c : s) {
        hashByte(h, static_cast<uint8_t>(c));
    }
}

static void hashVector(uint64_t& h, const std::vector<uint64_t>& v) {
    for (auto x : v) {
        hashU64(h, x);
    }
}

}

bool validatePositions(
    const ContestPositions& positions,
    uint64_t nx,
    uint64_t ny,
    uint64_t nz,
    uint32_t randomCount,
    uint32_t continuousCount,
    std::string& error
) {
    auto checkRandom = [&](const std::vector<uint64_t>& v,
                           const std::string& name,
                           uint64_t dim) -> bool {
        if (v.empty()) return true;
        if (v.size() != randomCount) {
            error = name + " random count " + std::to_string(v.size()) +
                    " != " + std::to_string(randomCount);
            return false;
        }
        std::unordered_set<uint64_t> seen;
        for (auto x : v) {
            if (x >= dim) {
                error = name + " random index " + std::to_string(x) +
                        " >= " + std::to_string(dim);
                return false;
            }
            if (seen.count(x)) {
                error = name + " random duplicate " + std::to_string(x);
                return false;
            }
            seen.insert(x);
        }
        return true;
    };

    auto checkContinuous = [&](const std::vector<uint64_t>& v,
                               const std::string& name,
                               uint64_t dim) -> bool {
        if (v.empty()) return true;
        if (v.size() != continuousCount) {
            error = name + " continuous count " + std::to_string(v.size()) +
                    " != " + std::to_string(continuousCount);
            return false;
        }
        for (size_t i = 0; i < v.size(); ++i) {
            if (v[i] >= dim) {
                error = name + " continuous index " + std::to_string(v[i]) +
                        " >= " + std::to_string(dim);
                return false;
            }
            if (i > 0 && v[i] != v[i-1] + 1) {
                error = name + " continuous not sequential at " +
                        std::to_string(v[i-1]) + "," + std::to_string(v[i]);
                return false;
            }
        }
        return true;
    };

    const bool hasAnyRequest =
        !positions.x_random.empty() || !positions.y_random.empty() ||
        !positions.z_random.empty() || !positions.x_continuous.empty() ||
        !positions.y_continuous.empty() || !positions.z_continuous.empty();
    if (!hasAnyRequest) {
        error = "positions file contains no slice requests";
        return false;
    }

    if (!checkRandom(positions.x_random, "X", nx)) return false;
    if (!checkRandom(positions.y_random, "Y", ny)) return false;
    if (!checkRandom(positions.z_random, "Z", nz)) return false;
    if (!checkContinuous(positions.x_continuous, "X", nx)) return false;
    if (!checkContinuous(positions.y_continuous, "Y", ny)) return false;
    if (!checkContinuous(positions.z_continuous, "Z", nz)) return false;
    return true;
}

bool parsePositionsFile(
    const std::string& path,
    uint64_t nx,
    uint64_t ny,
    uint64_t nz,
    uint32_t randomCount,
    uint32_t continuousCount,
    ContestPositions& output,
    std::string& error
) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        error = "cannot open positions file: " + path;
        return false;
    }

    ContestPositions pos;
    std::string line;
    int lineNum = 0;
    while (std::getline(ifs, line)) {
        ++lineNum;
        std::string axis, type;
        uint64_t index;
        std::string parseErr;
        ParseLineResult res = parseLine(line, lineNum, axis, type, index, parseErr);
        if (res == ParseLineResult::Error) {
            error = parseErr;
            return false;
        }
        if (res == ParseLineResult::Skip) continue;

        if (axis == "x" && type == "random") pos.x_random.push_back(index);
        else if (axis == "y" && type == "random") pos.y_random.push_back(index);
        else if (axis == "z" && type == "random") pos.z_random.push_back(index);
        else if (axis == "x" && type == "continuous") pos.x_continuous.push_back(index);
        else if (axis == "y" && type == "continuous") pos.y_continuous.push_back(index);
        else if (axis == "z" && type == "continuous") pos.z_continuous.push_back(index);
        else {
            error = "positions.csv line " + std::to_string(lineNum) +
                    ": unknown axis/type: " + axis + "/" + type;
            return false;
        }
    }

    if (!validatePositions(pos, nx, ny, nz, randomCount, continuousCount, error)) {
        return false;
    }

    output = std::move(pos);
    return true;
}

bool generateRandomPositions(
    uint64_t nx,
    uint64_t ny,
    uint64_t nz,
    uint32_t randomCount,
    uint32_t continuousCount,
    uint64_t seed,
    ContestPositions& output,
    std::string& error
) {
    ContestPositions pos;
    std::mt19937_64 rng(seed);

    auto genRandom = [&](uint64_t dim) -> std::vector<uint64_t> {
        std::vector<uint64_t> all(dim);
        std::iota(all.begin(), all.end(), 0);
        std::shuffle(all.begin(), all.end(), rng);
        all.resize(std::min<uint64_t>(randomCount, dim));
        return all;
    };

    auto genContinuous = [&](uint64_t dim) -> std::vector<uint64_t> {
        uint64_t maxStart = dim > continuousCount ? dim - continuousCount : 0;
        std::uniform_int_distribution<uint64_t> dist(0, maxStart);
        uint64_t start = dist(rng);
        std::vector<uint64_t> v(continuousCount);
        for (uint32_t i = 0; i < continuousCount; ++i)
            v[i] = start + i;
        return v;
    };

    pos.x_random = genRandom(nx);
    pos.y_random = genRandom(ny);
    pos.z_random = genRandom(nz);
    pos.x_continuous = genContinuous(nx);
    pos.y_continuous = genContinuous(ny);
    pos.z_continuous = genContinuous(nz);

    if (!validatePositions(pos, nx, ny, nz, randomCount, continuousCount, error)) {
        return false;
    }

    output = std::move(pos);
    return true;
}

uint64_t computePositionsHash(const ContestPositions& positions) {
    uint64_t h = 14695981039346656037ULL;

    hashString(h, "x_random");
    hashU64(h, static_cast<uint64_t>(positions.x_random.size()));
    for (auto x : positions.x_random) hashU64(h, x);

    hashString(h, "y_random");
    hashU64(h, static_cast<uint64_t>(positions.y_random.size()));
    for (auto y : positions.y_random) hashU64(h, y);

    hashString(h, "z_random");
    hashU64(h, static_cast<uint64_t>(positions.z_random.size()));
    for (auto z : positions.z_random) hashU64(h, z);

    hashString(h, "x_continuous");
    hashU64(h, static_cast<uint64_t>(positions.x_continuous.size()));
    for (auto x : positions.x_continuous) hashU64(h, x);

    hashString(h, "y_continuous");
    hashU64(h, static_cast<uint64_t>(positions.y_continuous.size()));
    for (auto y : positions.y_continuous) hashU64(h, y);

    hashString(h, "z_continuous");
    hashU64(h, static_cast<uint64_t>(positions.z_continuous.size()));
    for (auto z : positions.z_continuous) hashU64(h, z);

    return h;
}

} // namespace erwt3d
