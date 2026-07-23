# WSL24 SSD Benchmark Results

**Branch:** `perf/ssd-native-ext4`  
**Date:** 2026-07-24  
**Commit:** HEAD  
**Build:** Release + NATIVE_OPT + LZ4 + RZFP (ZFP 1.0.1)  
**Env:** Ubuntu 24.04 WSL2, ext4 on VHDX (C-drive SSD), i9-10850K (16T)

## Storage Budget

| Dataset | Format | Storage Ratio | Size |
|---------|--------|---------------|------|
| small (801×2405×2501) | LZ4 + XP stride=2 | 0.479x | ~9.2 GB |
| big (2001×2201×3000) | RZFP | 0.421x | ~22.2 GB |

Both within ≤1.50x limit. ✓

## Correctness

- 29/29 CTest pass
- RZFP max_relative_error < 1e-3
- RZFP violation_count = 0

## Performance: Small LZ4+XP

| Profile | Cache | Median T_composite | CV |
|---------|-------|-------------------|-----|
| SSD | Warm | 1.75s | 20% |
| SSD | Cold (drop_caches) | 1.49s | 7% |
| HDD | Cold (drop_caches) | 1.80s | 2% |

SSD warm higher variance likely due to page cache warmup.

## Performance: Big RZFP

| Profile | Cache | Median T_composite | CV |
|---------|-------|-------------------|-----|
| HDD ★ | Warm | 19.4s | 4% |
| HDD ★ | Cold (drop_caches) | 19.7s | 3% |
| SSD | Warm | 30.1s | — |
| SSD | Cold (drop_caches) | 30.6s | 1% |

★ Recommended configuration.

## Key Finding

**HDD profile (128 MB windows with window cache) outperforms SSD profile (4 MB windows, concurrent reads) for RZFP files even on SSD storage.**

Root cause: the RZFP window cache provides superblock-level spatial locality by caching entire superblock reads. The SSD profile reads individual leaf extents, trading away this locality for concurrency. On this dataset, the spatial locality benefit dominates.

For LZ4 compressed files, SSD and HDD profiles are comparable (1.5s vs 1.8s, SSD slightly faster).

## Recommended Final Configurations

### Small data (LZ4+XP):
```bash
./build-rel/erwt3d_contest --input small_lz4.erwt3d \
  --output-dir OUT --threads 8 --io-profile ssd
```
T_composite ≈ 1.5s cold, 1.8s warm

### Big data (RZFP):
```bash
./build-rel/erwt3d_contest --input big_rzfp.rzfp \
  --output-dir OUT --threads 8 --io-profile hdd
```
T_composite ≈ 19.7s cold

## Notes

- WSL2 ext4 VHDX on SSD provides ~100-130 MB/s sequential read
- Page cache warm/cold difference is negligible for RZFP (reads ~22 GB, system RAM 62 GB)
- Auto-detection correctly identifies WSL + ext4; falls back to HDD when rotational info unavailable (safe default)
- RZFP window cache stays enabled (default) for both profiles
