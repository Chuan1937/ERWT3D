#pragma once

#include "format.hpp"
#include "rzfp_codec.hpp"

#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace erwt3d {

constexpr char RZFP_MAGIC[8] = {'E', 'R', 'W', 'T', '3', 'D', 'R', '\0'};
constexpr uint32_t RZFP_VERSION = 1;

#pragma pack(push, 1)
struct RzfpFileHeader {
    char magic[8];
    uint32_t version;
    uint64_t nx, ny, nz;
    uint32_t dtype;
    uint32_t super_x;
    uint32_t super_y;
    uint32_t super_z;
    uint32_t leaf_x;
    uint32_t leaf_y;
    uint32_t leaf_z;
    uint64_t data_offset;
    uint64_t descriptor_offset;
    uint64_t payload_offset;
    uint64_t flags;
    uint64_t reserved[20];
};
#pragma pack(pop)

static_assert(sizeof(RzfpFileHeader) == 256, "RZFP header must be 256 bytes");

#pragma pack(push, 1)
struct RzfpSuperblockIndex {
    uint64_t payload_offset;
    uint32_t payload_bytes;
    uint32_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(RzfpSuperblockIndex) == 16, "RZFP superblock index must be 16 bytes");

using RzfpLeafDescriptor = uint16_t;

constexpr uint16_t RZFP_DESCRIPTOR_SIZE_MASK = 0x1FFFu;
constexpr uint16_t RZFP_DESCRIPTOR_CODEC_SHIFT = 13u;

inline RzfpLeafDescriptor makeDescriptor(RzfpLeafCodec codec, uint16_t record_size) {
    if (record_size > RZFP_DESCRIPTOR_SIZE_MASK) {
        throw std::overflow_error("RZFP record exceeds descriptor capacity");
    }
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(codec) << RZFP_DESCRIPTOR_CODEC_SHIFT) |
        record_size
    );
}

inline RzfpLeafCodec descriptorCodec(RzfpLeafDescriptor descriptor) {
    return static_cast<RzfpLeafCodec>(descriptor >> RZFP_DESCRIPTOR_CODEC_SHIFT);
}

inline uint16_t descriptorSize(RzfpLeafDescriptor descriptor) {
    return descriptor & RZFP_DESCRIPTOR_SIZE_MASK;
}

inline void initRzfpHeader(RzfpFileHeader& header) {
    std::memset(&header, 0, sizeof(header));
    std::memcpy(header.magic, RZFP_MAGIC, 8);
    header.version = RZFP_VERSION;
    header.dtype = DTYPE_FLOAT32;
    header.super_x = DEFAULT_SUPER_X;
    header.super_y = DEFAULT_SUPER_Y;
    header.super_z = DEFAULT_SUPER_Z;
    header.leaf_x = DEFAULT_LEAF_X;
    header.leaf_y = DEFAULT_LEAF_Y;
    header.leaf_z = DEFAULT_LEAF_Z;
}

inline bool validateRzfpHeader(const RzfpFileHeader& header) {
    if (std::memcmp(header.magic, RZFP_MAGIC, 8) != 0) return false;
    if (header.version != RZFP_VERSION) return false;
    if (header.dtype != DTYPE_FLOAT32) return false;
    if (header.super_x == 0 || header.super_y == 0 || header.super_z == 0) return false;
    if (header.leaf_x == 0 || header.leaf_y == 0 || header.leaf_z == 0) return false;
    if (header.super_x % header.leaf_x != 0) return false;
    if (header.super_y % header.leaf_y != 0) return false;
    if (header.super_z % header.leaf_z != 0) return false;
    return true;
}

inline bool hasRawXAux(const RzfpFileHeader& h) { return (h.flags & FLAG_HAS_RAW_X_AUX) != 0; }
inline uint64_t rzfpRawXAuxOffset(const RzfpFileHeader& h) { return h.reserved[0]; }
inline uint64_t rzfpRawXAuxBytes(const RzfpFileHeader& h) { return h.reserved[1]; }
inline uint64_t rzfpRawXAuxPlaneBytes(const RzfpFileHeader& h) { return h.reserved[2]; }
inline uint32_t rzfpRawXAuxVersion(const RzfpFileHeader& h) { return static_cast<uint32_t>(h.reserved[3]); }

inline uint64_t rzfpSuperGridX(const RzfpFileHeader& h) {
    return (h.nx + h.super_x - 1) / h.super_x;
}
inline uint64_t rzfpSuperGridY(const RzfpFileHeader& h) {
    return (h.ny + h.super_y - 1) / h.super_y;
}
inline uint64_t rzfpSuperGridZ(const RzfpFileHeader& h) {
    return (h.nz + h.super_z - 1) / h.super_z;
}
inline uint64_t rzfpTotalSuperblocks(const RzfpFileHeader& h) {
    return rzfpSuperGridX(h) * rzfpSuperGridY(h) * rzfpSuperGridZ(h);
}
inline uint64_t rzfpLeafsPerSuperX(const RzfpFileHeader& h) {
    return h.super_x / h.leaf_x;
}
inline uint64_t rzfpLeafsPerSuperY(const RzfpFileHeader& h) {
    return h.super_y / h.leaf_y;
}
inline uint64_t rzfpLeafsPerSuperZ(const RzfpFileHeader& h) {
    return h.super_z / h.leaf_z;
}
inline uint64_t rzfpTotalLeafsPerSuper(const RzfpFileHeader& h) {
    return rzfpLeafsPerSuperX(h) * rzfpLeafsPerSuperY(h) * rzfpLeafsPerSuperZ(h);
}
inline uint64_t rzfpTotalLeaves(const RzfpFileHeader& h) {
    return rzfpTotalSuperblocks(h) * rzfpTotalLeafsPerSuper(h);
}
inline uint64_t rzfpRawSize(const RzfpFileHeader& h) {
    return h.nx * h.ny * h.nz * sizeof(float);
}

inline uint64_t rzfpDescriptorOffset(const RzfpFileHeader& h) {
    return h.descriptor_offset;
}
inline uint64_t rzfpPayloadOffset(const RzfpFileHeader& h) {
    return h.payload_offset;
}

inline uint64_t rzfpSuperblockId(
    const RzfpFileHeader& h,
    uint64_t sz, uint64_t sy, uint64_t sx,
    PhysicalOrder order = PhysicalOrder::ZYX
) {
    const uint64_t sgX = rzfpSuperGridX(h);
    const uint64_t sgY = rzfpSuperGridY(h);
    const uint64_t sgZ = rzfpSuperGridZ(h);
    if (order == PhysicalOrder::V05_YZX) {
        return (sy * sgZ + sz) * sgX + sx;
    }
    return (sz * sgY + sy) * sgX + sx;
}

inline uint64_t rzfpSuperblockDescriptorOffset(
    const RzfpFileHeader& h,
    uint64_t sb_id
) {
    return h.descriptor_offset +
           sb_id * sizeof(RzfpLeafDescriptor) * rzfpTotalLeafsPerSuper(h);
}

} // namespace erwt3d
