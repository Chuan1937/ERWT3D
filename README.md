# ERWT3D

Efficient reading and writing of three-dimensional spatial data

## Overview

ERWT3D is a C++ library and command-line toolset for efficient read/write access to large regular 3D float32 volumes. It uses a custom single-file format with Morton-ordered physical layout to provide balanced performance for X, Y, and Z slice access.

## Features

- **Single-copy storage**: No redundant copies for different axes
- **Balanced performance**: Optimized for X, Y, and Z slice access
- **Morton ordering**: Spatial locality for better cache performance
- **Multi-threaded**: Parallel I/O and processing
- **Memory control**: Configurable memory limits
- **Cache support**: Optional LRU cache for repeated access

## Building

### Prerequisites

- C++17 compiler
- CMake 3.16+
- POSIX-compliant system (Linux, macOS)

### Build Commands

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Command-Line Tools

### erwt3d_info

Display information about an ERWT3D file:

```bash
./build/tools/erwt3d_info data.erwt3d
```

### erwt3d_convert

Convert between raw and ERWT3D formats:

```bash
# Raw to ERWT3D
./build/tools/erwt3d_convert \
  --input data.raw \
  --output data.erwt3d \
  --nx 1024 --ny 1024 --nz 512 \
  --threads 8 \
  --memory-limit-mb 2048

# ERWT3D to raw
./build/tools/erwt3d_convert \
  --input data.erwt3d \
  --output restored.raw \
  --to-raw \
  --threads 8 \
  --memory-limit-mb 2048
```

### erwt3d_slice

Read arbitrary slices:

```bash
# Read Z slice
./build/tools/erwt3d_slice \
  --input data.erwt3d \
  --axis z \
  --index 100 \
  --output z100.raw

# Read X line
./build/tools/erwt3d_slice \
  --input data.erwt3d \
  --line-x \
  --y 100 --z 200 \
  --output line_x.raw
```

### erwt3d_verify

Verify correctness:

```bash
./build/tools/erwt3d_verify \
  --raw data.raw \
  --erwt3d data.erwt3d \
  --nx 1024 --ny 1024 --nz 512 \
  --samples 100000
```

### erwt3d_bench

Run benchmarks:

```bash
./build/tools/erwt3d_bench \
  --input data.erwt3d \
  --output-dir bench_out \
  --random-count 100 \
  --continuous-count 10 \
  --threads 16 \
  --memory-limit-mb 2048 \
  --seed 20260511
```

## File Format

ERWT3D uses a custom binary format:

- **Header**: 256 bytes with magic, dimensions, block sizes
- **Data**: Superblocks in Morton order, each containing leaf blocks in Morton order

### Default Block Sizes

- Superblock: 64 × 64 × 64 float32 = 1 MiB
- Leaf block: 4 × 4 × 4 float32 = 256 bytes

## Storage Layout

Data is organized in Morton order (Z-order curve):

```
superblock_physical_id = morton3D(super_x, super_y, super_z)
leaf_physical_id = morton3D(leaf_x, leaf_y, leaf_z)
```

This provides:
- Balanced access for all axes
- Good spatial locality
- Formula-based offset calculation

## Performance

### Key Optimizations

1. **Extent merging**: Adjacent reads are merged
2. **Multi-threaded I/O**: Parallel pread/preadv
3. **LRU cache**: Optional cache for repeated access
4. **Morton ordering**: Balanced axis performance

### Benchmark Results

See [docs/benchmark.md](docs/benchmark.md) for detailed results.

## Documentation

- [Design Document](docs/design.md): Overall architecture
- [Index Documentation](docs/index.md): Computed offset indexing
- [Implementation Document](docs/implementation.md): Writer/reader pipelines
- [Benchmark Document](docs/benchmark.md): Performance analysis

## License

BSD 3-Clause License

## Contributing

1. Fork the repository
2. Create a feature branch
3. Commit your changes
4. Push to the branch
5. Create a Pull Request