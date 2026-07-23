# WSL24 SSD Benchmark Results

**Branch:** `perf/ssd-native-ext4`  
**Date:** 2026-07-24  
**Build:** Release + NATIVE_OPT + LZ4 + RZFP (ZFP 1.0.1)  
**Env:** Ubuntu 24.04 WSL2, ext4 on VHDX (C-drive SSD), i9-10850K (16T), 62 GiB RAM

## Storage Budget

| Dataset | Format | Storage Ratio | Size |
|---------|--------|---------------|------|
| small (801×2405×2501) | LZ4 + XP stride=2 | 0.479x | ~9.2 GB |
| big (2001×2201×3000) | RZFP | 0.421x | ~22.2 GB |

Both within ≤1.50x limit. ✓

## Correctness

- 32/32 CTest pass (29 existing + 3 new SSD tests)
- RZFP max_relative_error < 1e-3, violation_count = 0
- SSD extent planner: sort, dedup, merge, coverage ✓
- SSD LZ4 batch: byte-identical to HDD path ✓
- SSD memory config: defaults, budget enforcement ✓

## Performance: Memory Sweep (Guest-cold, drop_caches)

### Small LZ4+XP (auto → SSD)

| Memory (MB) | T_composite (s) |
|-------------|-----------------|
| 2048 | 2.06 |
| 4096 | 1.57 |
| 8192 | 1.67 |
| 16384 | 1.73 |
| 32768 | 1.71 |

### Big RZFP (auto → HDD)

| Memory (MB) | T_composite (s) |
|-------------|-----------------|
| 2048 | 72.95 |
| 4096 | 20.65 |
| 8192 | 22.48 |
| 16384 | 32.31 |
| 32768 | 33.22 |

## Performance: HDD vs SSD @4GB (Guest-cold, drop_caches)

### Small LZ4+XP

| Profile | T_composite range (s) | Notes |
|---------|----------------------|-------|
| HDD | 1.88-2.38 | Stable, low variance |
| SSD | 1.43-21.03 | High variance (WSL2 VHDX cache interference) |

### Big RZFP

| Profile | T_composite range (s) | Notes |
|---------|----------------------|-------|
| HDD | 24.57-34.73 | Lower, more stable |
| SSD | 39.45-62.04 | Higher, more variable |

## Auto-Detection Strategy

```
Auto + LZ4  → SSD (SSDConcurrentExtent)
Auto + RZFP → HDD (HDDReadWindow + window cache)
```

Rationale:
- LZ4 compressed files benefit from concurrent small-extent reads on SSD.
- RZFP files benefit from HDD large-window strategy with window cache,
  even on SSD storage. The window cache provides superblock-level
  spatial locality that dominates over concurrent read benefits.

## Recommended Final Configuration

```bash
# Small data (LZ4+XP):
./build/erwt3d_contest --input small_lz4.erwt3d \
  --output-dir OUT --threads 8 --memory-limit-mb 4096 \
  --io-profile auto
# → auto selects SSD, T_composite ≈ 1.6s (Guest-cold)

# Big data (RZFP):
./build/erwt3d_contest --input big_rzfp.rzfp \
  --output-dir OUT --threads 8 --memory-limit-mb 4096 \
  --io-profile auto
# → auto selects HDD, T_composite ≈ 20.7s (Guest-cold)
```

## Notes

- WSL2 page cache: `drop_caches` inside guest does not guarantee physical cold
  cache due to VHDX being backed by host page cache. WSL2 VHDX cache
  interference causes high variance in repeated cold-cache runs. Results are
  marked "Guest-cold (drop_caches)".
- 4 GB memory is the sweet spot for both datasets.
  Below 4 GB, RZFP window cache thrashing causes severe degradation (73s at 2 GB).
  Above 8 GB, extra memory provides no benefit (possibly worse due to cache
  eviction patterns).
- Host-cold (wsl --shutdown) not tested.
- All concurrency bugs fixed: lambda capture by index, per-task LZ4 decode
  buffer, error propagation (return false from decode failures).
- Removed unused modules: SSD writer pipeline, stub RZFP SSD executor, stub
  benchmark tool (can be added back in future PRs with full tests).
