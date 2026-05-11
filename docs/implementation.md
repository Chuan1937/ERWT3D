# ERWT3D Implementation Document

## Writer Pipeline

The writer converts raw float32 data to ERWT3D format:

1. **Initialize Header**
   - Set magic bytes, version, dimensions
   - Configure block sizes (default: 64^3 superblock, 4^3 leaf block)

2. **Process Superblocks**
   - Iterate through superblock grid in Z-Y-X order
   - For each superblock:
     a. Extract data from raw volume
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
   - Merge extents for efficient I/O
   - Read data using pread/preadv
   - Unpack leaf blocks to output buffer

3. **Full Volume Reading**
   - Read entire data region
   - Process each superblock
   - Unpack leaf blocks to correct positions

## Threading Model

### Thread Pool

```cpp
class ThreadPool {
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    // ...
};
```

### Pipeline Stages

1. **Slice Planning**: Single-threaded
2. **Extent Merging**: Single-threaded
3. **I/O Operations**: Multi-threaded (pread/preadv)
4. **Data Unpacking**: Multi-threaded
5. **Output Writing**: Single-threaded

### Thread Safety

- Thread pool manages task distribution
- Each thread has local buffers
- Cache uses mutex for thread safety
- File descriptors use pread (thread-safe)

## Memory Limit Control

Memory usage is divided into:

1. **I/O Buffers**: For reading extents
2. **Output Slice Buffer**: For slice data
3. **Cache**: Optional LRU cache for leaf blocks
4. **Thread-Local Buffers**: Temporary storage

### Configuration

```bash
--memory-limit-mb 2048
```

The implementation:
- Checks memory requirements before allocation
- Falls back to chunked processing if needed
- Respects global memory budget

## Cache Strategy

### LRU Cache

```cpp
class LeafCache {
    size_t maxSize;
    std::list<CacheEntry> lruList;
    std::unordered_map<uint64_t, iterator> index;
};
```

### Cache Operations

1. **Get**: Check if leaf block is cached
2. **Put**: Store leaf block (evict if necessary)
3. **Clear**: Remove all cached entries

### Cache Integration

- Cache is checked before I/O
- Cache is updated after I/O
- Cache respects memory limit
- Cache is optional (size = 0 disables)

## Error Handling

### Validation

- Header magic and version
- Dimension consistency
- Block size divisibility

### Recovery

- Graceful degradation on I/O errors
- Clear error messages
- Return error codes to caller

## Performance Optimizations

1. **Extent Merging**: Reduce syscall overhead
2. **Multi-threaded I/O**: Parallel pread/preadv
3. **Cache**: Reduce repeated I/O
4. **Morton Ordering**: Balanced axis performance
5. **Leaf Block Size**: 256 bytes for good read amplification