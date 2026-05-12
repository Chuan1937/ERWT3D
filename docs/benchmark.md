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

```csv
axis,mode,index,backend,threads,sb_parallel_mode,superblocks_touched,pread_calls,bytes_read,output_bytes,plan_time_ms,read_time_ms,unpack_time_ms,total_time_ms
```

Profile data confirms:
- I/O time (pread) dominates: ~90% of total for serial mode
- Unpack/copy is negligible: ~3-5%
- Plan time is constant: ~5-17ms per slice
- Parallel-read reduces I/O time proportionally with thread count

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
| Backend comparison | `docs/results/official_backend_comparison.csv` |
| Storage & correctness | `docs/results/official_storage_correctness.csv` |
| Thread/cache matrix | `docs/results/official_thread_cache_matrix.csv` |

## One-Command Reproduction

```bash
scripts/run_real_bench.sh data.raw NX NY NZ benchmarks/real
```

## Optimization Opportunities

1. **I/O Optimization**: Direct I/O, io_uring for async I/O, or mmap could further reduce syscall overhead
2. **Pipeline mode**: Producer-consumer pipeline (read→unpack→write stages) for overlapping I/O with CPU
3. **NUMA awareness**: Thread pinning and local CPU memory allocation for multi-socket systems
4. **Memory-mapped files**: Could eliminate copy overhead entirely
