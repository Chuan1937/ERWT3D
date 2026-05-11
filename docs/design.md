# ERWT3D Design Document

## Overview

ERWT3D is a C++ library for efficient read/write access to large regular 3D float32 volumes. It uses a custom single-file format with Morton-ordered physical layout to provide balanced performance for X, Y, and Z slice access.

## Single-Copy Storage Principle

Unlike approaches that store redundant copies for each axis, ERWT3D uses a single-copy storage strategy:

- Data is stored once in Morton order
- No X/Y/Z redundant copies
- Storage size remains close to raw size (< 1.5x)
- First version targets ~1.0x + small header/padding

## Superblock Design

The volume is divided into superblocks:

```
superblock = 64 x 64 x 64 float32 = 1 MiB
```

Rationale:
- Large enough for efficient I/O operations
- Small enough for thread scheduling and cache management
- Aligns well with typical memory page sizes

## Leaf Block Design

Each superblock is further divided into leaf blocks:

```
leaf block = 4 x 4 x 4 float32 = 256 bytes
```

Rationale:
- Provides balanced read amplification for X/Y/Z slice access
- Small enough for fine-grained access
- Large enough to amortize overhead

## Morton Physical Layout

Morton ordering (Z-order curve) is used within each superblock to arrange leaf blocks:

```
leaf_physical_id = morton3D(lb_x, lb_y, lb_z)
```

Superblocks are arranged in sequential Z-Y-X row-major order to avoid sparse file holes when the superblock grid dimensions are not powers of two.

```
superblock_offset = (sz * gridY + sy) * gridX + sx
```

Benefits:
- Morton ordering within superblocks provides balanced axis access
- Sequential superblock layout eliminates sparse holes for non-power-of-two grids
- Enables formula-based offset calculation

## Boundary Handling

When dimensions are not divisible by block sizes:

- Boundary blocks are padded with zeros
- Restored raw output matches original dimensions exactly
- Padding is acceptable but storage ratio must remain < 1.5x

## Storage Ratio Formula

```
storage_ratio = file_size / (nx * ny * nz * 4)
```

Target: < 1.5x for the first version.

## File Format

The file consists of:

1. **Header** (256 bytes)
   - Magic bytes: "ERWT3D\0"
   - Version: 1
   - Dimensions: nx, ny, nz
   - Data type: float32
   - Block sizes: super_x/y/z, leaf_x/y/z
   - Data offset
   - Reserved fields

2. **Data Area**
   - Superblocks in Morton order
   - Each superblock contains leaf blocks in Morton order

## Offset Calculation

File offset for a specific element:

```
superblock_id = morton3D(super_x, super_y, super_z)
leaf_id = morton3D(leaf_x, leaf_y, leaf_z)
offset = data_offset + superblock_id * superblock_bytes + leaf_id * leaf_bytes
```

No explicit index table is needed - offsets are computed by formula.

## Threading Model

- Single-machine, single-process, multi-threaded
- Thread pool for parallel I/O and processing
- Configurable memory limit via `--memory-limit-mb`

## Cache Strategy

Optional LRU cache for leaf blocks:

- Key: global leaf block id or file offset
- Value: 256-byte leaf block
- Configurable size via `--cache-mb`
- Respects global memory limit