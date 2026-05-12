# ERWT3D

Efficient reading and writing of three-dimensional spatial data

## Overview

ERWT3D is a C++ library and command-line toolset for efficient read/write access to large regular 3D float32 volumes. It uses a custom single-file format with Morton-ordered physical layout to provide balanced performance for X, Y, and Z slice access.

## Features

- **Single-copy storage**: No redundant copies for different axes
- **Balanced performance**: Optimized for X, Y, and Z slice access
- **Morton leaf ordering**: Balanced axis access within superblocks
- **Multi-threaded I/O**: Parallel pread via thread pool (`--threads`)
- **Memory-bounded batches**: Respects `--memory-limit-mb` for slice reads
- **LRU leaf cache**: Reuse leaf blocks across continuous slices (`--cache-mb`)
- **Streaming restore**: `readFullToFile` writes directly without full allocation

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
# Final recommended command for competition submission:
./build/erwt3d_bench \
  --input data.erwt3d \
  --output-dir bench_out \
  --random-count 100 \
  --continuous-count 10 \
  --threads 1 \
  --memory-limit-mb 8192 \
  --cache-mb 0 \
  --io-backend sb \
  --seed 20260511
```

Outputs `bench_result.csv` (summary) and `bench_detail.csv` (per-slice timing).

### erwt3d_verify

Verify correctness (supports streaming sampling for large datasets):

```bash
./build/erwt3d_verify \
  --raw data.raw \
  --erwt3d data.erwt3d \
  --nx 2001 --ny 2201 --nz 3000 \
  --samples 100000
```

## File Format

ERWT3D uses a custom binary format:

- **Header**: 256 bytes with magic, dimensions, block sizes
- **Data**: Superblocks in Morton order, each containing leaf blocks in Morton order

### Default Block Sizes

- Superblock: 64 × 64 × 64 float32 = 1 MiB
- Leaf block: 4 × 4 × 4 float32 = 256 bytes

## Storage Layout

Data is organized in two levels:

- **Superblocks**: arranged in Z-Y-X row-major order (sequential)
- **Leaf blocks**: within each superblock, arranged in Morton order (Z-order curve)

```text
superblock_offset = (sz * gridY + sy) * gridX + sx
leaf_offset  = morton3D(lx, ly, lz) * leaf_bytes
```

This provides:
- Balanced leaf-level access for all axes via Morton ordering
- Simple sequential superblock layout avoids sparse holes for non-power-of-two grids

## Performance

### Key Optimizations

1. **Superblock I/O backend**: Reads whole 1 MiB superblocks, reducing syscall count by 100–800x vs per-extent pread
2. **Morton ordering**: Balanced leaf-level access for all axes
3. **Single-threaded recommended**: Mutex contention outweighs parallelism on real data
4. **No cache needed**: File system page cache handles sequential access; app-level cache adds overhead

### Official Benchmark Results (100 random + 10 continuous slices)

| Dataset | Backend | Threads | T_total | Storage Ratio | Correctness |
|---------|---------|---------|---------|---------------|-------------|
| **20G** (801x2405x2501) | **sb** | **1** | **249ms** | 1.075x | passed |
| **50G** (2001x2201x3000) | **sb** | **1** | **770ms** | 1.044x | passed |
| 20G | pread | 1 | DNF | — | — |

**SB backend is the recommended choice for competition submission.** PRead backend times out on real data (~389k syscalls per X-slice).

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