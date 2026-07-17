#pragma once

#include "format.hpp"
#include "raw_x_aux.hpp"
#include <cstdint>
#include <string>

namespace erwt3d {

bool writeERWT3D(const std::string& outputPath,
                 const float* rawData,
                 uint64_t nx, uint64_t ny, uint64_t nz,
                 uint32_t superX = DEFAULT_SUPER_X,
                 uint32_t superY = DEFAULT_SUPER_Y,
                 uint32_t superZ = DEFAULT_SUPER_Z,
                 uint32_t leafX = DEFAULT_LEAF_X,
                 uint32_t leafY = DEFAULT_LEAF_Y,
                 uint32_t leafZ = DEFAULT_LEAF_Z,
                 int numThreads = 1,
                 size_t memoryLimitMB = 2048,
                 uint32_t panelAxis = 0,
                 uint32_t panelStride = 0);

bool writeERWT3DFromFile(const std::string& outputPath,
                         const std::string& inputPath,
                         uint64_t nx, uint64_t ny, uint64_t nz,
                         uint32_t superX = DEFAULT_SUPER_X,
                         uint32_t superY = DEFAULT_SUPER_Y,
                         uint32_t superZ = DEFAULT_SUPER_Z,
                         uint32_t leafX = DEFAULT_LEAF_X,
                         uint32_t leafY = DEFAULT_LEAF_Y,
                         uint32_t leafZ = DEFAULT_LEAF_Z,
                         int numThreads = 1,
                         size_t memoryLimitMB = 2048,
                         uint32_t panelAxis = 0,
                         uint32_t panelStride = 0,
                         bool compress = false,
                         RawXAuxMode rawXAuxMode = RawXAuxMode::Off,
                         bool forceStorageEdge = false,
                         RawXAuxStats* rawXAuxStats = nullptr);

bool appendRawXAuxToFile(const std::string& erwt3dPath,
                         const std::string& rawPath,
                         uint64_t nx, uint64_t ny, uint64_t nz,
                         RawXAuxStats* stats = nullptr,
                         bool forceEdge = false);

} // namespace erwt3d
