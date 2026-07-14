# RZFP File Format

Version: 1 (experimental)

## Layout

```text
[ RzfpFileHeader          256 bytes ]
[ RzfpSuperblockIndex × total_superblocks  16 bytes each ]
[ LeafDescriptor × total_leaves            2 bytes each ]
[ Variable leaf record payloads ]
```

Only **one** copy of the 3D volume is stored.

## Header

```c
#pragma pack(push, 1)
struct RzfpFileHeader {
    char     magic[8];          // "ERWT3DR\0"
    uint32_t version;           // 1
    uint64_t nx, ny, nz;        // original dimensions
    uint32_t dtype;             // 1 = float32
    uint32_t super_x;           // default 64
    uint32_t super_y;           // default 64
    uint32_t super_z;           // default 64
    uint32_t leaf_x;            // default 4
    uint32_t leaf_y;            // default 4
    uint32_t leaf_z;            // default 4
    uint64_t data_offset;       // reserved
    uint64_t descriptor_offset; // start of descriptor region
    uint64_t payload_offset;    // start of variable payload region
    uint64_t flags;             // FLAG_PHYSICAL_ORDER_YZX etc.
    uint64_t reserved[20];
};
#pragma pack(pop)
```

`sizeof(RzfpFileHeader) == 256`.

## Superblock index

```c
#pragma pack(push, 1)
struct RzfpSuperblockIndex {
    uint64_t payload_offset;    // absolute file offset of first leaf record
    uint32_t payload_bytes;     // total compressed payload bytes for this SB
    uint32_t reserved;
};
#pragma pack(pop)
```

`sizeof(RzfpSuperblockIndex) == 16`.

## Leaf descriptor

Each descriptor is a 16-bit value:

```
bits 13..15 : codec enum (RzfpLeafCodec)
bits  0..12 : record_size in bytes (payload only, excluding the 2-byte descriptor)
```

Maximum record size: 8191 bytes (larger records are impossible because raw is
only 256 bytes).

Descriptors are stored in physical superblock order.  Within each superblock
the 4096 descriptors follow the Morton order of the leaves.

## Leaf record layouts

### RawFloat32

```text
256 bytes : 64 raw float32 values
```

### ConstantZero

Empty payload.  All 64 decoded values are `0.0f`.

### ConstantValue

```text
4 bytes : float32 value
```

All 64 decoded values equal this value.

### ZfpAccuracy

```text
1 byte  : int8 min_exp
N bytes : ZFP compressed payload
```

The absolute tolerance used for ZFP is `2^min_exp`.

### ZfpAccuracyExceptions

```text
1 byte  : int8 min_exp
1 byte  : exception_count
8 bytes : exception_mask (uint64)
N bytes : ZFP compressed payload
M bytes : exception_count × float32 exception values
```

Exception values are stored in bit-order from lowest to highest bit of
`exception_mask`.

### ZfpPrecision

```text
1 byte  : uint8 precision
N bytes : ZFP compressed payload
```

## Physical ordering

Descriptors and payloads are stored in the physical superblock order defined by
`flags & FLAG_PHYSICAL_ORDER_YZX`:

- `ZYX`: `sb_id = (sz * sgY + sy) * sgX + sx`
- `V05_YZX`: `sb_id = (sy * sgZ + sz) * sgX + sx`

Within a superblock leaves are always in Morton order.
