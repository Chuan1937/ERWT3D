# ERWT3D Index Documentation

## Computed Offset Indexing

ERWT3D uses computed offsets instead of explicit index tables. This approach:

- Reduces file size (no large index overhead)
- Simplifies implementation
- Enables fast random access

## Morton ID Calculation

### 3D to 1D Morton Encoding

```cpp
uint64_t morton3D(uint32_t x, uint32_t y, uint32_t z);
```

The Morton code interleaves the bits of x, y, and z coordinates:

```
x: x2 x1 x0
y: y2 y1 y0
z: z2 z1 z0

morton: z2 y2 x2 z1 y1 x1 z0 y0 x0
```

### Usage in ERWT3D

1. **Superblock ID**: `morton3D(super_x, super_y, super_z)`
2. **Leaf ID within superblock**: `morton3D(leaf_x, leaf_y, leaf_z)`

## Slice Compiler

The slice compiler converts a slice request into:

1. **Touched superblocks**: Which superblocks intersect the slice
2. **Touched leaf blocks**: Which leaf blocks within each superblock
3. **File offsets and sizes**: For I/O operations
4. **Copy plans**: How to extract data from leaf buffers to output

### Example: Z-Slice at index z=100

1. Determine which superblock contains z=100
2. Determine which leaf block within that superblock
3. Calculate file offsets for all touched leaf blocks
4. Generate copy instructions to extract the Z-plane

## Extent Merging

Adjacent file reads are merged to reduce syscall overhead:

```
Before merging:
  offset=1000, size=256
  offset=1256, size=256
  offset=1512, size=256

After merging:
  offset=1000, size=768
```

This optimization:
- Reduces number of system calls
- Improves I/O throughput
- Minimizes overhead for small reads

## Why No Large Explicit Index

Traditional formats often use large index tables to map logical coordinates to physical locations. ERWT3D avoids this by:

1. Using formula-based offset calculation
2. Leveraging Morton ordering properties
3. Accepting small padding for boundary blocks

Benefits:
- Smaller file size
- Simpler implementation
- No index maintenance overhead

## Memory-Mapped Access

For read-only access, the file can be memory-mapped:

```cpp
void* mmap(int fd, size_t length, int prot, int flags, off_t offset);
```

This enables:
- Virtual memory management by OS
- Automatic caching
- Simplified code for random access