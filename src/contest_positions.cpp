#include "erwt3d/contest_positions.hpp"

#include <algorithm>
#include <cmath>
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

static bool parseLine(const std::string& line,
                      std::string& axis, std::string& type, uint64_t& index) {
    std::string l = line;
    while (!l.empty() && (l.back() == '\r' || l.back() == '\n' || l.back() == ' '))
        l.pop_back();
    if (l.empty() || l[0] == '#') return false;

    for (auto& c : l) if (c == ',') c = ' ';

    std::istringstream iss(l);
    std::string a, t;
    uint64_t idx;
    if (!(iss >> a >> t >> idx)) return false;
    axis = axisName(a);
    for (auto& c : t) c = static_cast<char>(std::tolower(c));
    type = t;
    index = idx;
    return true;
}

static uint64_t fnv1a64(const uint8_t* data, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void hashVector(uint64_t& h, const std::vector<uint64_t>& v) {
    for (auto x : v) {
        h = fnv1a64(reinterpret_cast<const uint8_t*>(&x), sizeof(x));
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
    while (std::getline(ifs, line)) {
        std::string axis, type;
        uint64_t index;
        if (!parseLine(line, axis, type, index)) continue;

        if (axis == "x" && type == "random") pos.x_random.push_back(index);
        else if (axis == "y" && type == "random") pos.y_random.push_back(index);
        else if (axis == "z" && type == "random") pos.z_random.push_back(index);
        else if (axis == "x" && type == "continuous") pos.x_continuous.push_back(index);
        else if (axis == "y" && type == "continuous") pos.y_continuous.push_back(index);
        else if (axis == "z" && type == "continuous") pos.z_continuous.push_back(index);
        else {
            error = "unknown axis/type: " + axis + "/" + type + " in line: " + line;
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
    hashVector(h, positions.x_random);
    hashVector(h, positions.y_random);
    hashVector(h, positions.z_random);
    hashVector(h, positions.x_continuous);
    hashVector(h, positions.y_continuous);
    hashVector(h, positions.z_continuous);
    return h;
}

}
