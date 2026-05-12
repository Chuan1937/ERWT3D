# ERWT3D Benchmark Document

## Benchmark Method

The benchmark follows the competition scoring style:

1. **Random Slice Test**
   - 100 random slice reads for each axis (X, Y, Z)
   - Random indices generated with fixed seed
   - Includes output write time

2. **Continuous Slice Test**
   - 10 continuous slice reads for each axis
   - Slices are adjacent in memory
   - Tests cache effectiveness

## Random Slice Test

### Procedure

1. Generate 100 random indices for each axis
2. For each index:
   - Read slice from ERWT3D file
   - Write slice to output file
   - Measure total time (read + write)

### Metrics

- Average time per slice
- Minimum time
- Maximum time
- Total time for 100 slices

## Continuous Slice Test

### Procedure

1. Select 10 adjacent slices for each axis
2. For each slice:
   - Read slice from ERWT3D file
   - Write slice to output file
   - Measure total time

### Metrics

- Average time per slice
- Minimum time
- Maximum time
- Total time for 10 slices

## Correctness Verification

### Lossless Verification

For lossless float32 storage:

```
max_abs_error = 0
max_rel_error = 0
num_failed = 0
passed = true
```

### Sampling Verification

For large datasets, verify with random samples:

```bash
./erwt3d_verify --raw data.raw --erwt3d data.erwt3d \
  --nx 1024 --ny 1024 --nz 512 \
  --samples 100000
```

## Storage Ratio

### Calculation

```
storage_ratio = file_size / (nx * ny * nz * 4)
```

### Target

- First version: < 1.5x
- Optimal: ~1.0x + small header/padding

### Example

```
Raw size: 1024 * 1024 * 512 * 4 = 2 GB
ERWT3D file: 2.1 GB
Storage ratio: 2.1 / 2.0 = 1.05x
```

## Current Measured Results

### Test Environment
- CPU: Intel i7-13700F, 24 threads (WSL2)
- RAM: 64 GB
- OS: Fedora Linux 43
- Compiler: g++ 15.2.1

### Synthetic 256×256×256

| Config | X rand | Y rand | Z rand | T_random_avg | X cont | Y cont | Z cont | T_total | Balance |
|--------|--------|--------|--------|-------------|--------|--------|--------|---------|---------|
| ERWT3D t1 | 5.22 | 3.03 | 1.92 | 3.39 | 4.52 | 2.83 | 2.05 | 3.26 | 2.72× |
| ERWT3D t8 | 69.01 | 53.51 | 18.98 | 47.17 | 70.56 | 40.49 | 21.06 | 45.60 | 3.64× |
| Raw row-major | 19.12 | 0.70 | 0.49 | 6.77 | 20.08 | 0.43 | 0.75 | 6.93 | 39.0× |

### CUP Real Data (small: 801×2405×2501)

| Metric | Value |
|--------|-------|
| Raw size | 18.0 GB |
| ERWT3D size | 19.3 GB |
| Storage ratio | 1.075× |
| Correctness (100k samples) | passed=true, max_abs_error=0 |
| Full benchmarks | Infeasible at current I/O throughput (~389k pread/slice) |

### Analysis

- **Storage**: Well within 1.5× target (1.075× for CUP, 1.000× for aligned cubic)
- **Balance**: ERWT3D is 14× more balanced than raw row-major (2.72 vs 39.0)
- **Thread scaling**: Performance decreases with threads (mutex contention); single-threaded recommended for current implementation
- **Bottleneck**: Per-extent pread() syscall overhead; ~1000+ calls per 256³ slice, ~120k per CUP Z-slice. Next: preadv() batched reads

## Benchmark Commands

### Run Benchmark

```bash
./build/tools/erwt3d_bench \
  --input data.erwt3d \
  --output-dir bench_out \
  --random-count 100 \
  --continuous-count 10 \
  --threads 16 \
  --memory-limit-mb 2048 \
  --cache-mb 512 \
  --seed 20260511
```

### Output

- `bench_result.csv`: Aggregated summary statistics
- `bench_detail.csv`: Per-slice timing (axis, mode, iteration, index, time_ms, output_bytes, threads, cache_mb, memory_limit_mb)

### Cache Control

```bash
--cache-mb 0     # disable cache
--cache-mb 512   # 512 MB LRU cache for leaf blocks
```

### Baseline Comparison

For raw row-major baseline, read slices directly from a raw float32 file and compare timing. Expected: Z slices fast (contiguous), X/Y slices slow (random I/O). ERWT3D should show more balanced X/Y/Z performance.

## Optimization Opportunities

1. **I/O Optimization**
   - Use io_uring for async I/O
   - Implement read-ahead
   - Use direct I/O

2. **Cache Optimization**
   - Increase cache size
   - Implement prefetching
   - Use adaptive cache policies

3. **Threading Optimization**
   - Increase thread count
   - Implement work stealing
   - Optimize task granularity

4. **Memory Optimization**
   - Use memory-mapped files
   - Implement streaming processing
   - Reduce memory copies