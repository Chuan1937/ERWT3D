# E2E Timing Baseline — cup 20GB / 50GB

**Branch:** `perf/ssd-native-ext4`  
**Date:** 2026-07-24  
**Env:** WSL2, ext4 VHDX on C-drive SSD, i9-10850K (16T), 62 GB RAM

## Corrected Timing

T_composite = process_e2e / 6. Total time for all 330 output files = process_e2e.

Previous results (T_composite) understated real time by ~6x.

## Small 20GB (LZ4+XP, 0.479x, SSD profile, 4GB)

Guest-cold x5:

| Run | T_composite (s) | process_e2e (s) |
|-----|----------------:|----------------:|
| 1 (true cold) | 2.21 | 13.35 |
| 2 | 1.55 | 9.34 |
| 3 | 1.52 | 9.16 |
| 4 | 1.67 | 10.11 |
| 5 | 1.66 | 10.06 |

Median process_e2e (exc. round 1): **~9.8s**  
Round 1 (true Guest-cold): **13.35s**

Total is already fast. No aggressive optimization needed.

## Big 50GB (RZFP, 0.421x, HDD profile, 4GB)

Guest-cold x5:

| Run | T_composite (s) | process_e2e (s) | merged_read (s) | total_write (s) |
|-----|----------------:|----------------:|----------------:|----------------:|
| 1 (true cold) | 20.71 | 124.25 | 121.46 | 2.79 |
| 2 | 18.78 | 112.70 | 109.89 | 2.80 |
| 3 | 19.25 | 115.49 | 113.07 | 2.42 |
| 4 | 18.78 | 112.68 | 109.99 | 2.69 |
| 5 | 19.22 | 115.32 | 112.51 | 2.81 |

Median process_e2e: **~115s**  
T_composite median: **~19.2s**

### Time Decomposition (median)

| Phase | Time (s) | Fraction |
|-------|---------:|---------:|
| merged_read (I/O + decode + scatter) | 112.5 | 97.6% |
| total_write | 2.7 | 2.3% |
| total_create_files | 0.005 | ~0% |
| **process_e2e** | **115.3** | **100%** |

### Key Finding

**Output writing is only ~2.4-2.8s (2.3% of total).**  
This means:
- mmap output path: negligible improvement
- Streaming writer pipeline: negligible improvement
- File creation/preallocation: negligible improvement

**The bottleneck is I/O + decode + scatter at ~112s.**  
This means:
- Faster codec (FastMantissa, FastPRel) → direct benefit
- AxisPack (fewer leaf reads, sequential access) → direct benefit
- RZFP decode optimization → direct benefit

### Next Steps Priority

1. FastMantissa codec → diagnose if ZFP decode is the bottleneck
2. AxisPack format → convert random cross-superblock reads to sequential plane reads
3. If AxisPack reduces read amplification, even constant-bitrate codec may suffice
