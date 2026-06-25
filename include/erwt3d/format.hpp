#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace erwt3d {

// Magic bytes for ERWT3D format
constexpr char ERWT3D_MAGIC[8] = {'E', 'R', 'W', 'T', '3', 'D', '\0', '\0'};
constexpr uint32_t ERWT3D_VERSION = 1;
constexpr uint32_t DTYPE_FLOAT32 = 1;

constexpr uint64_t FLAG_HAS_X_PANELS = 1ULL << 0;
constexpr uint64_t FLAG_HAS_Y_PANELS = 1ULL << 1;
constexpr uint64_t FLAG_HAS_Z_PANELS = 1ULL << 2;
constexpr uint64_t FLAG_HAS_X_PLANES = 1ULL << 3;

// Default block sizes
constexpr uint32_t DEFAULT_SUPER_X = 64;
constexpr uint32_t DEFAULT_SUPER_Y = 64;
constexpr uint32_t DEFAULT_SUPER_Z = 64;
constexpr uint32_t DEFAULT_LEAF_X = 4;
constexpr uint32_t DEFAULT_LEAF_Y = 4;
constexpr uint32_t DEFAULT_LEAF_Z = 4;

#pragma pack(push, 1)
struct ERWT3DHeader {
    char magic[8];              // "ERWT3D\0"
    uint32_t version;           // 1
    uint64_t nx, ny, nz;        // original dimensions
    uint32_t dtype;             // 1 = float32
    uint32_t super_x;           // default 64
    uint32_t super_y;           // default 64
    uint32_t super_z;           // default 64
    uint32_t leaf_x;            // default 4
    uint32_t leaf_y;            // default 4
    uint32_t leaf_z;            // default 4
    uint64_t data_offset;       // start of data area
    uint64_t flags;             // compression/checksum/etc. reserved
    uint64_t reserved[22];
};
#pragma pack(pop)

static_assert(sizeof(ERWT3DHeader) == 256, "Header must be 256 bytes");
static_assert(offsetof(ERWT3DHeader, magic) == 0);
static_assert(offsetof(ERWT3DHeader, version) == 8);
static_assert(offsetof(ERWT3DHeader, nx) == 12);
static_assert(offsetof(ERWT3DHeader, data_offset) == 64);

inline void initHeader(ERWT3DHeader& header) {
    std::memset(&header, 0, sizeof(header));
    std::memcpy(header.magic, ERWT3D_MAGIC, 8);
    header.version = ERWT3D_VERSION;
    header.dtype = DTYPE_FLOAT32;
    header.super_x = DEFAULT_SUPER_X;
    header.super_y = DEFAULT_SUPER_Y;
    header.super_z = DEFAULT_SUPER_Z;
    header.leaf_x = DEFAULT_LEAF_X;
    header.leaf_y = DEFAULT_LEAF_Y;
    header.leaf_z = DEFAULT_LEAF_Z;
    header.data_offset = sizeof(ERWT3DHeader);
}

inline bool validateHeader(const ERWT3DHeader& header) {
    if (std::memcmp(header.magic, ERWT3D_MAGIC, 8) != 0) return false;
    if (header.version != ERWT3D_VERSION) return false;
    if (header.dtype != DTYPE_FLOAT32) return false;
    if (header.super_x == 0 || header.super_y == 0 || header.super_z == 0) return false;
    if (header.leaf_x == 0 || header.leaf_y == 0 || header.leaf_z == 0) return false;
    if (header.super_x % header.leaf_x != 0) return false;
    if (header.super_y % header.leaf_y != 0) return false;
    if (header.super_z % header.leaf_z != 0) return false;
    if (header.data_offset < sizeof(ERWT3DHeader)) return false;
    return true;
}

inline uint64_t getSuperblockBytes(const ERWT3DHeader& header) {
    return static_cast<uint64_t>(header.super_x) * header.super_y * header.super_z * sizeof(float);
}

inline uint64_t getLeafBytes(const ERWT3DHeader& header) {
    return static_cast<uint64_t>(header.leaf_x) * header.leaf_y * header.leaf_z * sizeof(float);
}

inline uint64_t getRawSize(const ERWT3DHeader& header) {
    return header.nx * header.ny * header.nz * sizeof(float);
}

inline uint64_t getSuperGridX(const ERWT3DHeader& header) {
    return (header.nx + header.super_x - 1) / header.super_x;
}

inline uint64_t getSuperGridY(const ERWT3DHeader& header) {
    return (header.ny + header.super_y - 1) / header.super_y;
}

inline uint64_t getSuperGridZ(const ERWT3DHeader& header) {
    return (header.nz + header.super_z - 1) / header.super_z;
}

inline uint64_t getTotalSuperblocks(const ERWT3DHeader& header) {
    return getSuperGridX(header) * getSuperGridY(header) * getSuperGridZ(header);
}

inline uint64_t getLeafsPerSuperX(const ERWT3DHeader& header) {
    return header.super_x / header.leaf_x;
}

inline uint64_t getLeafsPerSuperY(const ERWT3DHeader& header) {
    return header.super_y / header.leaf_y;
}

inline uint64_t getLeafsPerSuperZ(const ERWT3DHeader& header) {
    return header.super_z / header.leaf_z;
}

inline uint64_t getTotalLeafsPerSuper(const ERWT3DHeader& header) {
    return getLeafsPerSuperX(header) * getLeafsPerSuperY(header) * getLeafsPerSuperZ(header);
}

inline bool hasXPanels(const ERWT3DHeader& h) { return (h.flags & FLAG_HAS_X_PANELS) != 0; }
inline bool hasYPanels(const ERWT3DHeader& h) { return (h.flags & FLAG_HAS_Y_PANELS) != 0; }
inline bool hasZPanels(const ERWT3DHeader& h) { return (h.flags & FLAG_HAS_Z_PANELS) != 0; }
inline bool hasAnyPanels(const ERWT3DHeader& h) { return (h.flags & (FLAG_HAS_X_PANELS|FLAG_HAS_Y_PANELS|FLAG_HAS_Z_PANELS)) != 0; }
inline bool hasXPlanes(const ERWT3DHeader& h) { return (h.flags & FLAG_HAS_X_PLANES) != 0; }
inline uint64_t getXPlaneOffset(const ERWT3DHeader& h) { return h.reserved[16]; }
inline uint64_t getXPlaneCount(const ERWT3DHeader& h) { return h.reserved[17]; }
inline uint32_t getXPlaneStride(const ERWT3DHeader& h) { return h.reserved[18] ? static_cast<uint32_t>(h.reserved[18]) : 1; }

inline uint32_t getPanelStrideX(const ERWT3DHeader& h) { return static_cast<uint32_t>(h.reserved[0]); }
inline uint32_t getPanelStrideY(const ERWT3DHeader& h) { return static_cast<uint32_t>(h.reserved[1]); }
inline uint32_t getPanelStrideZ(const ERWT3DHeader& h) { return static_cast<uint32_t>(h.reserved[2]); }
inline uint64_t getPanelDataOffset(const ERWT3DHeader& h) { return h.reserved[3]; }
inline uint64_t getPanelIndexOffset(const ERWT3DHeader& h) { return h.reserved[4]; }
inline uint64_t getPanelStorageBytes(const ERWT3DHeader& h) { return h.reserved[5]; }

inline uint64_t panelPlaneBytes(const ERWT3DHeader& h) {
    return static_cast<uint64_t>(h.super_y) * h.super_z * sizeof(float);
}
inline uint64_t panelsPerSuperblock(const ERWT3DHeader& h, uint32_t stride) {
    return stride > 0 ? h.super_x / stride : 0;
}
inline uint64_t panelBytesPerSuperblock(const ERWT3DHeader& h, uint32_t stride) {
    return panelsPerSuperblock(h, stride) * panelPlaneBytes(h);
}

} // namespace erwt3d