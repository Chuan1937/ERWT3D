#pragma once

#include "format.hpp"
#include <cstdint>
#include <string>

namespace erwt3d {

// Write raw float32 data to ERWT3D format
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

// Write raw float32 data from file to ERWT3D format
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
                         bool rawXAux = false,
                         bool forceStorageEdge = false);

enum class RawXAuxMode { Auto, On, Off };

struct RawXAuxStats {
    uint64_t raw_x_aux_bytes = 0;
    uint64_t raw_x_aux_offset = 0;
    double total_storage_ratio = 0.0;
    bool stored = false;
};

bool appendRawXAuxToFile(const std::string& erwt3dPath,
                         const std::string& rawPath,
                         uint64_t nx, uint64_t ny, uint64_t nz,
                         RawXAuxStats* stats = nullptr,
                         bool forceEdge = false);

} // namespace erwt3d