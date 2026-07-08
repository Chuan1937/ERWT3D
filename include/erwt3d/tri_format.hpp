#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace erwt3d {

constexpr char TRI_MAGIC[8] = {'E','R','W','T','3','D','T','\0'};
constexpr uint32_t TRI_VERSION = 1;

constexpr uint32_t TRI_CODEC_RAW = 0;
constexpr uint32_t TRI_CODEC_ZFP_FIXED_RATE = 1;
constexpr uint32_t TRI_CODEC_LZ4 = 2;

constexpr uint32_t TRI_FLAG_HAS_EXCEPTION = 1u << 0;

#pragma pack(push, 1)
struct TriHeader {
    char     magic[8];              // 8
    uint32_t version;               // 4
    uint64_t nx, ny, nz;            // 24
    uint32_t block_x, block_y, block_z; // 12
    uint32_t codec;                 // 4
    uint32_t rate_bpv;              // 4
    uint32_t flags;                 // 4
    uint64_t data_offset;           // 8
    uint64_t axis_offsets[3];       // 24
    uint64_t axis_block_counts[3];  // 24
    uint64_t axis_block_bytes[3];   // 24
    uint64_t exception_index_offset; // 8
    uint64_t exception_data_offset;  // 8
    uint64_t exception_count;        // 8
    uint64_t reserved[11];           // 88
    uint32_t _padding;               // 4
    // Total: 8+4+24+12+4+4+4+8+24+24+24+8+8+8+88+4 = 256
};
#pragma pack(pop)
static_assert(sizeof(TriHeader) == 256, "TriHeader must be 256 bytes");

inline void initTriHeader(TriHeader& h) {
    std::memset(&h, 0, sizeof(h));
    std::memcpy(h.magic, TRI_MAGIC, 8);
    h.version = TRI_VERSION;
    h.block_x = 4;
    h.block_y = 4;
    h.block_z = 4;
    h.data_offset = sizeof(TriHeader);
}

inline bool validateTriHeader(const TriHeader& h) {
    if (std::memcmp(h.magic, TRI_MAGIC, 8) != 0) return false;
    if (h.version != TRI_VERSION) return false;
    if (h.nx == 0 || h.ny == 0 || h.nz == 0) return false;
    if (h.block_x == 0 || h.block_y == 0 || h.block_z == 0) return false;
    return true;
}

inline uint64_t triBlockCountX(const TriHeader& h) { return (h.nx + h.block_x - 1) / h.block_x; }
inline uint64_t triBlockCountY(const TriHeader& h) { return (h.ny + h.block_y - 1) / h.block_y; }
inline uint64_t triBlockCountZ(const TriHeader& h) { return (h.nz + h.block_z - 1) / h.block_z; }

// For ZFP fixed-rate: each block is rate * 64 bits = rate * 8 bytes
inline uint64_t triZfpBlockBytes(double rate_bpv) {
    return static_cast<uint64_t>(rate_bpv * 64.0 / 8.0);
}

// Exception index entry: maps (bx,by,bz) to raw block data offset
#pragma pack(push, 1)
struct TriExceptionIndex {
    uint64_t block_id;   // encoded as (bz * byCount + by) * bxCount + bx
    uint64_t data_offset;
};
#pragma pack(pop)
static_assert(sizeof(TriExceptionIndex) == 16);

} // namespace erwt3d
