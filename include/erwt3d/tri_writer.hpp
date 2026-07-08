#pragma once

#include "tri_format.hpp"
#include <string>
#include <cstdint>

namespace erwt3d {

bool writeTriAxisLayout(const std::string& rawPath,
                        const std::string& outPath,
                        uint64_t nx, uint64_t ny, uint64_t nz,
                        uint32_t codec, uint32_t rate_bpv,
                        double relTol, double zeroAbsTol,
                        int numThreads, size_t memoryLimitMB);

} // namespace erwt3d
