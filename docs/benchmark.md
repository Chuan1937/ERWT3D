# ERWT3D Benchmark Document

## Benchmark Method

The benchmark follows the competition scoring style:

1. **Random Slice Test**: 100 random slice reads per axis (X, Y, Z), random indices with fixed seed
2. **Continuous Slice Test**: 10 continuous adjacent slice reads per axis

Both include output write time. The competition composite time is:

```
actual_composite_time = combined X/Y/Z random + continuous slice read/write time
```

## Scoring Formula

```text
performance_score = (baseline_time / actual_composite_time) * 60
```

## Correctness Verification

All ERWT3D conversions are verified lossless using streaming random sampling (100k samples):
- `max_abs_error = 0`
- `max_rel_error = 0`
- `num_failed = 0`
- `passed = true`

Parallel-read mode is bit-identical to serial SB (verified with `cmp` on full slices).

See [erwt3d_verify](../tools/erwt3d_verify.cpp) for the streaming verify tool (memory-efficient, works on 50GB+ datasets).

## Test Environment

- CPU: Intel i7-13700F, 24 threads (WSL2)
- RAM: 64 GB
- OS: Fedora Linux 43
- Compiler: g++ 15.2.1

## SB Parallel Modes (Issue #12)

### Why Previous Multithreading Failed

The old `--threads` model (PR #9) had per-extent thread dispatch with shared mutex. On real data:

```text
20G SB t1: T_total = 248.87 ms (best)
20G SB t8: T_total = 248.71 ms (tied, but worse X-axis balance)
50G SB t1: T_total = 770.13 ms (best)
50G SB t8: T_total = 1008.30 ms (1.3x slower!)
```

Root causes:
1. Thread granularity was too fine (per-leaf or per-extent)
2. Shared mutex contention in hot paths
3. Threads competed for page-cache access instead of increasing I/O queue depth
4. No proper work partitioning for superblock-level I/O

### New Solution: `--sb-parallel-mode parallel-read`

The new `parallel-read` mode partitions superblock tasks among threads:

```text
for each thread:
    owns its own 1 MiB superblock buffer (no shared buffers)
    reads assigned superblocks via pread
    extracts leaf data to disjoint output regions (no locking)
```

Key design:
- Work unit = superblock (1 MiB), not leaf (256B) or extent
- Per-thread buffer eliminates shared buffer contention
- Disjoint output regions eliminate mutex on writes
- Coarse task granularity reduces scheduling overhead

### Phase Profiling (`--profile-io`)

The `--profile-io` flag writes per-slice I/O phase timing to `io_profile.csv`:

```
axis,mode,index,backend,threads,sb_parallel_mode,superblocks_touched,pread_calls,bytes_read,output_bytes,plan_time_ms,read_time_wall_ms,unpack_time_wall_ms,read_time_sum_ms,unpack_time_sum_ms,total_time_ms
```

- `read_time_wall_ms`: max thread read time (wall clock) for parallel mode; identical to sum for serial
- `unpack_time_wall_ms`: max thread unpack time (wall clock) for parallel mode
- `read_time_sum_ms` / `unpack_time_sum_ms`: total CPU time across all threads

Profile findings:
- I/O time (pread) dominates: ~90% of total for serial mode
- Unpack/copy is negligible: ~3-5%
- Plan time is constant: ~5-17ms per slice
- Parallel-read reduces wall-clock I/O time by dividing superblocks among threads; sum I/O increases slightly due to contention

### 50G Cold vs Warm Cache

The 50G reduced-count benchmark (20+5, T_total=736ms) was run before full benchmark warmup and reflects cold page-cache conditions. The full 100+10 benchmark (T_total=300ms) benefits from page-cache warming over many slices. All final benchmark CSV entries are annotated with cache conditions.

## Official Benchmark Results

### 20G Data (801x2405x2501, 18.0 GB raw)

| Metric | Value |
|--------|-------|
| Storage ratio | 1.075x |
| Correctness | passed (100k samples, max_abs_error=0) |

| Config | T_x_rand | T_y_rand | T_z_rand | T_rand_avg | T_cont_avg | T_total | Speedup |
|--------|----------|----------|----------|------------|------------|---------|---------|
| SB serial t1 (old) | 373ms | 149ms | 199ms | 240ms | 257ms | 249ms | 1.00x |
| **SB parallel-read t8 (new)** | **155ms** | **45ms** | **40ms** | **80ms** | **66ms** | **73ms** | **3.41x** |

### 50G Data (2001x2201x3000, 50.4 GB raw)

| Metric | Value |
|--------|-------|
| Storage ratio | 1.044x |
| Correctness | passed (100k samples, max_abs_error=0) |

| Config | T_x_rand | T_y_rand | T_z_rand | T_rand_avg | T_cont_avg | T_total | Speedup |
|--------|----------|----------|----------|------------|------------|---------|---------|
| SB serial t1 (old) | 2159ms | 941ms | 532ms | 1210ms | 330ms | 770ms | 1.00x |
| **SB parallel-read t8 (new)** | **499ms** | **563ms** | **377ms** | **480ms** | **121ms** | **300ms** | **2.57x** |

## Command Profiles

### Development / High-Resource Mode

For rapid iteration during development:

```bash
./build/erwt3d_bench \
  --input data.erwt3d \
  --output-dir dev_bench \
  --random-count 20 \
  --continuous-count 5 \
  --threads 8 \
  --memory-limit-mb 8192 \
  --cache-mb 0 \
  --io-backend sb \
  --sb-parallel-mode parallel-read \
  --profile-io \
  --seed 20260511
```

### Final Competition Mode

```bash
./build/erwt3d_bench \
  --input data.erwt3d \
  --output-dir final_bench \
  --random-count 100 \
  --continuous-count 10 \
  --threads 8 \
  --memory-limit-mb 8192 \
  --cache-mb 0 \
  --io-backend sb \
  --sb-parallel-mode parallel-read \
  --seed 20260511
```

### Key Settings

- `--io-backend sb`: Superblock I/O backend (only viable backend for real data)
- `--sb-parallel-mode parallel-read`: Partition superblocks among threads with per-thread buffers
- `--threads 8`: Recommended for competition (t8 gives 2.57x–3.41x speedup over serial)
- `--cache-mb 0`: App-level cache adds overhead without benefit for SB reads
- `--memory-limit-mb 8192`: Works under 8GB (each thread needs ~1 MiB buffer)

## Thread Scaling

Parallel-read mode scales well on real data:

| Dataset | t1 (serial) | t4 | t8 | Best Speedup |
|---------|-------------|-----|------|--------------|
| 20G (20+5) | 193ms | 110ms | 58ms | 3.34x |
| 50G (20+5) | — | — | 737ms | — |
| 20G (full 100+10) | 249ms | — | 73ms | 3.41x |
| 50G (full 100+10) | 770ms | — | 300ms | 2.57x |

X-axis random reads show the strongest scaling: 373ms→155ms (2.4x) on 20G, 2159ms→499ms (4.3x) on 50G.

## Storage Ratio

Both datasets are well within the 1.5x target:

| Dataset | Ratio | Notes |
|---------|-------|-------|
| 20G (801x2405x2501) | 1.075x | Non-cubic dimensions add boundary superblock waste |
| 50G (2001x2201x3000) | 1.044x | Larger volume, less boundary overhead ratio |
| Synthetic 256³ | 1.000x | Perfectly aligned cubic volume |

**Storage budget note:** The current 1.044x–1.075x ratio leaves significant headroom below the 1.5x limit. Future optimization may use additional index structures or layout metadata (up to <1.5x) if it improves random/continuous slice read speed.

## Source Data

All results are derived from committed CSV evidence files in `docs/results/`:

| Table | Source CSV |
|-------|-----------|
| 20G parallel modes | `docs/results/sb_parallel_modes_20g.csv` |
| 50G parallel modes | `docs/results/sb_parallel_modes_50g.csv` |
| Final settings comparison | `docs/results/sb_final_settings.csv` |
| Phase profile | `docs/results/sb_phase_profile.csv` |
| Panel storage | `docs/results/panel_storage_estimate.csv` |
| Panel benchmarks | `docs/results/panel_benchmark_20g.csv` |
| Panel correctness | `docs/results/panel_correctness.csv` |
| Panel comparison | `docs/results/final_vs_panel_comparison.csv` |
| Line benchmarks 20G | `docs/results/line_benchmark_20g.csv` |
| Line benchmarks 50G | `docs/results/line_benchmark_50g.csv` |
| Line correctness | `docs/results/line_correctness.csv` |

## X Micro-Panel Index (Issue #14)

### Motivation

SB backend reads complete 1 MiB superblocks. For an X-slice, each superblock contributes only a 16 KiB YZ plane — a 64x read amplification. Adding sparse X-plane panels uses the storage budget headroom (1.044x → 1.344x, still below 1.5x) to reduce X-slice I/O.

### Approach

Store every k-th local X-plane per superblock as a compact auxiliary panel. For stride=4, store 16 of 64 planes per superblock (256 KiB per superblock extra).

### Results (20G, parallel-read t8)

| Config | T_x_rand | T_total | Storage |
|--------|----------|---------|---------|
| No panels | 155ms | 73ms | 1.075x |
| X-panels stride=4 | 124ms | 65ms | 1.344x |

X-panels improve T_x_random by 20% and T_total by 11%, using +0.27x storage ratio. Panel reads use the same parallel-read infrastructure (per-thread pread of 16 KiB panels instead of 1 MiB superblocks).

Panel hits occur when `local_x % stride == 0` (25% of random X-slices for stride=4). Misses fall back to standard SB parallel-read.

### Conversion

```bash
./build/erwt3d_convert \
  --input data.raw \
  --output data_panel.erwt3d \
  --nx NX --ny NY --nz NZ \
  --threads 8 --memory-limit-mb 8192 \
  --panel-axis x --panel-stride 4
```

Panel files are backward compatible (old readers open without errors but ignore panel metadata). The `--panel-axis` flag accepts `x`, `y`, `z`; stride must divide the superblock size evenly.

## X/Y/Z Line Reads (Issue #18)

ERWT3D supports efficient single-line reads along all three axes via direct leaf-block access. Lines are not extracted from full 2D slices; only the specific 256B leaf blocks containing the line are read.

### API

```cpp
// Axis-generic: axis=X → fixed1=y,fixed2=z; axis=Y → fixed1=x,fixed2=z; axis=Z → fixed1=x,fixed2=y
bool readLine(SliceAxis axis, uint64_t fixed1, uint64_t fixed2, float* output,
              int numThreads = 1, size_t memoryLimitMB = 2048);

// Convenience wrappers
bool readLineX(uint64_t y, uint64_t z, float* output);
bool readLineY(uint64_t x, uint64_t z, float* output);
bool readLineZ(uint64_t x, uint64_t y, float* output);
```

### CLI

```bash
./build/erwt3d_line --input data.erwt3d --axis x --fixed1 Y --fixed2 Z --output line.raw
```

### Performance (20G, SB serial)

| Axis | Typical latency | Leaf blocks touched |
|------|----------------|---------------------|
| X | <0.1ms | ~13 (3.2KB) |
| Y | ~0.1ms | ~152 (38KB) |
| Z | ~0.1ms | ~160 (40KB) |

Lines are <1ms latency for all axes. Multi-threading does not help for line reads (task count is too small).

### Correctness

7 dimensions tested (17×19×23 through 100×100×100), boundaries (0, mid, last), wrapper vs generic API, serial vs parallel-read (threads 2/4/8). All 7/7 CTest pass, 0 errors.

## Optimization Opportunities

1. **I/O Optimization**: Direct I/O, io_uring for async I/O, or mmap could further reduce syscall overhead
2. **Pipeline mode**: Producer-consumer pipeline (read→unpack→write stages) for overlapping I/O with CPU
3. **NUMA awareness**: Thread pinning and local CPU memory allocation for multi-socket systems
4. **Memory-mapped files**: Could eliminate copy overhead entirely
