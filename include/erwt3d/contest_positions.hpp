#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace erwt3d {

struct ContestPositions {
    std::vector<uint64_t> x_random;
    std::vector<uint64_t> y_random;
    std::vector<uint64_t> z_random;
    std::vector<uint64_t> x_continuous;
    std::vector<uint64_t> y_continuous;
    std::vector<uint64_t> z_continuous;
};

enum class ParseLineResult {
    Skip,
    Parsed,
    Error,
};

bool parsePositionsFile(
    const std::string& path,
    uint64_t nx,
    uint64_t ny,
    uint64_t nz,
    uint32_t randomCount,
    uint32_t continuousCount,
    ContestPositions& output,
    std::string& error
);

bool generateRandomPositions(
    uint64_t nx,
    uint64_t ny,
    uint64_t nz,
    uint32_t randomCount,
    uint32_t continuousCount,
    uint64_t seed,
    ContestPositions& output,
    std::string& error
);

bool validatePositions(
    const ContestPositions& positions,
    uint64_t nx,
    uint64_t ny,
    uint64_t nz,
    uint32_t randomCount,
    uint32_t continuousCount,
    std::string& error
);

uint64_t computePositionsHash(const ContestPositions& positions);

}
