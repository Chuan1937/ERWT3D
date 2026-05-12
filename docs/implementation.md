# ERWT3D Implementation Document

## Writer Pipeline

The writer converts raw float32 data to ERWT3D format:

1. **Initialize Header**
   - Set magic bytes, version, dimensions
   - Configure block sizes (default: 64^3 superblock, 4^3 leaf block)

2. **Process Superblocks**
   - Iterate through superblock grid in Z-Y-X order
   - For each superblock:
     a. Extract data from raw volume (streaming from file)
     b. Fill superblock buffer (with padding if needed)
     c. Write leaf blocks in Morton order

3. **Leaf Block Writing**
   - For each leaf block within superblock:
     a. Calculate Morton ID
     b. Extract leaf data from superblock buffer
     c. Write to file at computed offset

## Reader Pipeline

The reader converts ERWT3D format to raw float32:

1. **Open File and Read Header**
   - Validate magic, version, dimensions
   - Initialize cache if requested

2. **Slice Reading**
   - Plan slice access using slice compiler
   - Prepare precomputed merged-extent mapping (once per slice)
   - Read extents in memory-bounded batches
   - Multi-threaded pread via thread pool
   - Unpack leaf blocks to output buffer

3. **Full Volume Reading**
   - Process one superblock at a time (streaming)
   - Unpack leaf blocks to correct positions
   - readFullToFile writes rows via pwrite without full allocation

## Memory Limit Control

Memory usage is divided into:

1. **I/O Buffers**: For reading extents (batched by memory limit)
2. **Output Slice Buffer**: For slice data (allocated by caller)
3. **Cache**: Optional LRU cache for individual leaf blocks (256 bytes each)
4. **Superblock Buffer**: For streaming restore (1 MiB default)

### Configuration

```bash
--memory-limit-mb 2048
```

The implementation:
- Checks superblock buffer requirements before allocation
- Batches merged extents into groups that fit within budget
- Falls back to single-extent reads if one extent alone exceeds budget

## Cache Strategy

### LRU Cache

Leaf blocks are cached by file offset. Each entry is 256 bytes.

### Cache Integration

- Cache is checked before I/O in readOneExtent()
- Size validation prevents stale reads from mismatched merged extents
- Cache respects memory limit
- `--cache-mb 0` disables, output identical to no-cache

### Multi-threaded I/O

- ThreadPool manages parallel extent reads via pread
- `--threads 1` uses sequential path
- `--threads > 1` parallelizes independent extent reads
- Results bit-identical to single-threaded output

## Performance Optimizations

1. **Extent Merging**: Reduce syscall overhead
2. **Precomputed Mapping**: O(1) lookup per copy instruction
3. **Multi-threaded pread**: Parallel extent reads via thread pool
4. **Memory-bounded batches**: Process large slices without full allocation
5. **Leaf block cache**: Reduce repeated I/O for continuous slices
6. **Morton Leaf Ordering**: Balanced axis performance