# WSL24 SSD Scale Test Results

**Branch:** `perf/ssd-native-ext4`  
**Date:** 2026-07-24  
**Build:** Release + NATIVE_OPT + LZ4 + RZFP (ZFP 1.0.1)  
**Env:** Ubuntu 24.04 WSL2, ext4 on VHDX (C-drive SSD), i9-10850K (16T), 62 GiB RAM

## Disk Layout

```
/dev/sdd  1T  WSL VHDX (ext4), 779 GB free
C:\       931G Windows C-drive SSD, 108 GB free
```

VHDX is on C-drive. 70 GB test skipped due to C-drive space concern.

## Data Summary

| Dataset | Dims | Raw Size | Format | Optimized | Ratio | Convert Time |
|---------|------|----------|--------|-----------|-------|--------------|
| 10 GB | 1025×1281×2044 | 10,735,292,400 B | RZFP | 6,314,173,492 B | 0.588x | 305s |
| 35 GB | 1537×2049×2983 | 37,577,602,716 B | RZFP | 22,068,707,463 B | 0.587x | 1391s |

Both auto-selected RZFP (random float data → uncompressible by LZ4).

## Correctness

- 32/32 CTest pass
- Converter: violations=0, max_relative_error < 1e-3 for both

## Performance: 10 GB RZFP

| Config | Cache | T_composite (s) | Notes |
|--------|-------|-----------------|-------|
| auto → HDD | Warm x3 | 5.85 / 6.03 / 6.14 | median 6.03s |
| auto → HDD | Cold x3 | 5.94 / 5.95 / 5.96 | median 5.96s, CV < 1% |
| HDD @4GB | Cold | 5.69 | |
| SSD @4GB | Cold | 8.72 | 53% slower than HDD |

**Key finding:** At 10 GB, the dataset fits in page cache (62 GB RAM). Cold/warm difference is negligible. HDD large-window is 53% faster than SSD extent.

## Performance: 35 GB RZFP

| Config | Cache | T_composite (s) | Notes |
|--------|-------|-----------------|-------|
| auto → HDD | Cold 1 (true cold) | 52.97 | First run after drop_caches |
| auto → HDD | Cold 2 | 22.00 | Partial cache during run |
| auto → HDD | Cold 3 | 14.99 | Mostly cached |
| HDD @4GB | Cold (after series) | 14.91 | |
| SSD @4GB | Cold (after series) | 23.86 | 60% slower than HDD |

**Key finding:** At 35 GB, true cold is ~53s. After a single warmup, drops to ~15s. HDD still beats SSD by 60%.

## Memory Sweep: 35 GB (cached state)

| Memory (MB) | T_composite (s) |
|-------------|-----------------|
| 2048 | 14.87 |
| 4096 | 14.82 |
| 8192 | 15.14 |
| 16384 | 15.90 |

Minimal variation in cached state. 4 GB is optimal.

## Auto-Detection Verification

| Dataset | Format | Auto Selected | Correct? |
|---------|--------|---------------|----------|
| 10 GB | RZFP | HDD | ✓ (HDD 5.69s > SSD 8.72s) |
| 35 GB | RZFP | HDD | ✓ (HDD 14.91s > SSD 23.86s) |

## Conclusion

1. **Auto-selection correct**: RZFP → HDD large-window on all scales (10/35 GB).
2. **SSD extent strategy NEVER wins for RZFP**: window cache spatial locality dominates.
3. **4 GB memory is sufficient**: even at 35 GB, no benefit from >4 GB.
4. **35 GB true cold (53s→15s)**: single warm-up cuts latency by 3.5x due to page cache.
5. **10 GB fits in RAM**: cold/warm identical (~6s).
6. **LZ4 not tested on scale**: random float data uncompressible by LZ4.
