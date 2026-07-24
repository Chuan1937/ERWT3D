#pragma once

#include "axis_plane.hpp"
#include "rzfp_format.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace erwt3d {

constexpr char RZFP_AXIS_LEAF_MAGIC[8] =
    {'E', 'R', 'W', 'T', 'R', 'A', 'L', '\0'};
constexpr uint32_t RZFP_AXIS_LEAF_VERSION = 1;

#pragma pack(push, 1)
struct RzfpAxisLeafHeader {
    char magic[8];
    uint32_t version;
    uint32_t axis;
    uint64_t nx;
    uint64_t ny;
    uint64_t nz;
    uint32_t leaf_x;
    uint32_t leaf_y;
    uint32_t leaf_z;
    uint32_t reserved0;
    uint64_t slab_count;
    uint64_t index_offset;
    uint64_t payload_offset;
    uint64_t source_payload_bytes;
    uint64_t descriptor_hash;
    uint64_t reserved[20];
};

struct RzfpAxisLeafSlabIndex {
    uint64_t offset;
    uint64_t bytes;
};
#pragma pack(pop)

static_assert(sizeof(RzfpAxisLeafHeader) == 256,
              "RZFP axis-leaf header must be 256 bytes");
static_assert(sizeof(RzfpAxisLeafSlabIndex) == 16,
              "RZFP axis-leaf index must be 16 bytes");

struct RzfpAxisLeafRepackStats {
    uint64_t source_payload_bytes = 0;
    std::array<uint64_t, 3> replica_bytes{};
    uint64_t metadata_bytes = 0;
    uint64_t total_bytes = 0;
    double storage_ratio = 0.0;
};

std::string rzfpAxisLeafPath(const std::string& metadataPath, PlaneAxis axis);

bool validateRzfpAxisLeafHeader(
    const RzfpAxisLeafHeader& sidecar,
    const RzfpFileHeader& source,
    PlaneAxis axis,
    uint64_t descriptorHash
);

bool repackRzfpAxisLeaves(
    const std::string& inputPath,
    const std::string& outputPath,
    size_t memoryLimitMiB = 1024,
    RzfpAxisLeafRepackStats* stats = nullptr
);

} // namespace erwt3d
