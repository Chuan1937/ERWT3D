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

- OS: Fedora Linux 43 (WSL)
- CPU: [To be filled]
- RAM: [To be filled]
- Storage: [To be filled]

### Results

```
T_x_random: [To be filled] ms
T_y_random: [To be filled] ms
T_z_random: [To be filled] ms
T_random_avg: [To be filled] ms

T_x_continuous: [To be filled] ms
T_y_continuous: [To be filled] ms
T_z_continuous: [To be filled] ms
T_cont_avg: [To be filled] ms

T_total: [To be filled] ms
Storage ratio: [To be filled]x
```

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
  --seed 20260511
```

### Output

- `bench_result.csv`: Detailed results
- Console output: Summary statistics

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