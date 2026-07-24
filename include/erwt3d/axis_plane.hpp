#pragma once

#include "format.hpp"
#include "slice.hpp"

#include <cstdint>
#include <cstring>
#include <string>

namespace erwt3d {

// ============================================================
// Unified axis definitions
// ============================================================

enum class PlaneAxis : uint8_t {
    X = 0,
    Y = 1,
    Z = 2,
};

inline SliceAxis axisToSliceAxis(PlaneAxis p) {
    switch (p) {
        case PlaneAxis::X: return SliceAxis::X;
        case PlaneAxis::Y: return SliceAxis::Y;
        case PlaneAxis::Z: return SliceAxis::Z;
    }
    return SliceAxis::X;
}

inline PlaneAxis sliceAxisToPlaneAxis(SliceAxis a) {
    switch (a) {
        case SliceAxis::X: return PlaneAxis::X;
        case SliceAxis::Y: return PlaneAxis::Y;
        case SliceAxis::Z: return PlaneAxis::Z;
    }
    return PlaneAxis::X;
}

inline const char* axisExtension(PlaneAxis axis) {
    switch (axis) {
        case PlaneAxis::X: return ".xp";
        case PlaneAxis::Y: return ".yp";
        case PlaneAxis::Z: return ".zp";
    }
    return ".xp";
}

inline const char* axisLabel(PlaneAxis axis) {
    switch (axis) {
        case PlaneAxis::X: return "X";
        case PlaneAxis::Y: return "Y";
        case PlaneAxis::Z: return "Z";
    }
    return "?";
}

// ============================================================
// AxisPlaneShape — describes one axial slice
// ============================================================

struct AxisPlaneShape {
    PlaneAxis axis = PlaneAxis::X;
    uint64_t plane_count = 0;  // number of planes along this axis
    uint64_t dim_a = 0;        // first varying dimension in output
    uint64_t dim_b = 0;        // second varying dimension in output
    uint64_t plane_elements = 0;
    uint64_t plane_bytes = 0;
};

inline AxisPlaneShape makeAxisPlaneShape(PlaneAxis axis,
                                          uint64_t nx, uint64_t ny, uint64_t nz) {
    AxisPlaneShape s;
    s.axis = axis;
    switch (axis) {
        case PlaneAxis::X:
            s.plane_count = nx;
            s.dim_a = ny;
            s.dim_b = nz;
            break;
        case PlaneAxis::Y:
            s.plane_count = ny;
            s.dim_a = nx;
            s.dim_b = nz;
            break;
        case PlaneAxis::Z:
            s.plane_count = nz;
            s.dim_a = nx;
            s.dim_b = ny;
            break;
    }
    s.plane_elements = s.dim_a * s.dim_b;
    s.plane_bytes = s.plane_elements * sizeof(float);
    return s;
}

// ============================================================
// AxisPlane sidecar file v2 header (256 bytes)
// ============================================================

constexpr char AXISPLANE_MAGIC[8] = {'E', 'R', 'W', 'A', 'X', 'P', '\0', '\0'};
constexpr uint32_t AXISPLANE_VERSION_V1 = 1;
constexpr uint32_t AXISPLANE_VERSION_V2 = 2;

constexpr uint8_t AXISPLANE_COMPRESSION_NONE = 0;
constexpr uint8_t AXISPLANE_COMPRESSION_LZ4 = 1;
constexpr uint8_t AXISPLANE_COMPRESSION_RZFP_2D = 2;

#pragma pack(push, 1)
struct AxisPlaneHeader {
    char magic[8];             // "ERWAXP\0\0"
    uint32_t version;          // 1=legacy X-only, 2=generic
    uint8_t axis;              // PlaneAxis value (v2+)
    uint8_t compression;       // 0=none, 1=LZ4, 2=RZFP_2D
    uint8_t padding0[10];

    uint64_t nx;
    uint64_t ny;
    uint64_t nz;

    uint64_t plane_count;      // number of planes in this sidecar
    uint64_t plane_elements;   // floats per plane (dim_a * dim_b)
    uint64_t index_offset;     // byte offset of index entries
    uint64_t data_offset;      // byte offset of first plane data

    // LZ4-specific fields (v2+)
    uint32_t chunk_rows;
    uint32_t chunks_per_plane;
    uint64_t total_chunks;
    uint64_t chunk_raw_bytes;
    uint64_t total_storage_bytes;

    // RZFP_2D-specific fields (v2+)
    uint8_t padding1[144];
};
#pragma pack(pop)

static_assert(sizeof(AxisPlaneHeader) == 256, "AxisPlaneHeader must be 256 bytes");

// ============================================================
// AxisPlane index entry (16 bytes)
// ============================================================

#pragma pack(push, 1)
struct AxisPlaneIndexEntry {
    uint64_t offset;           // file offset of this plane/record
    uint32_t compressed_size;  // stored size
    uint32_t raw_size;         // decompressed size (for LZ4) or 0 (for RZFP)
};
#pragma pack(pop)

static_assert(sizeof(AxisPlaneIndexEntry) == 16, "AxisPlaneIndexEntry must be 16 bytes");

// ============================================================
// Sidecar file path utilities
// ============================================================

inline std::string axisPlaneSidecarPath(const std::string& mainPath, PlaneAxis axis) {
    return mainPath + axisExtension(axis);
}

inline void initAxisPlaneHeader(AxisPlaneHeader& h) {
    std::memset(&h, 0, sizeof(h));
    std::memcpy(h.magic, AXISPLANE_MAGIC, 8);
    h.version = AXISPLANE_VERSION_V2;
}

inline bool validateAxisPlaneHeader(const AxisPlaneHeader& h,
                                     uint64_t expectedNx,
                                     uint64_t expectedNy,
                                     uint64_t expectedNz) {
    if (std::memcmp(h.magic, AXISPLANE_MAGIC, 8) != 0) return false;
    if (h.version > AXISPLANE_VERSION_V2) return false;
    if (h.axis > 2) return false; // X, Y, or Z only
    if (h.nx != expectedNx || h.ny != expectedNy || h.nz != expectedNz) return false;
    return true;
}

// ============================================================
// AxisPlane output layout helpers
// ============================================================

// Output layout for each axis (Z-fastest convention):
//   X plane: output[y * nz + z]
//   Y plane: output[x * nz + z]
//   Z plane: output[x * ny + y]

inline uint64_t axisPlaneOutputIndex(PlaneAxis axis,
                                      uint64_t a_idx, uint64_t b_idx,
                                      uint64_t dim_a, uint64_t dim_b) {
    (void)dim_b;
    switch (axis) {
        case PlaneAxis::X: return a_idx * dim_b + b_idx; // a_idx=y, b_idx=z
        case PlaneAxis::Y: return a_idx * dim_b + b_idx; // a_idx=x, b_idx=z
        case PlaneAxis::Z: return a_idx * dim_b + b_idx; // a_idx=x, b_idx=y
    }
    return 0;
}

} // namespace erwt3d
