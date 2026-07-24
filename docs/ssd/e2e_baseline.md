# E2E Timing Baseline — cup 20GB / 50GB

**Branch:** `perf/ssd-native-ext4`  
**Tag:** `v0.8-ssd-baseline`  
**Date:** 2026-07-24  
**Env:** WSL2, ext4 VHDX on C-drive SSD, i9-10850K (16T), 62 GB RAM  
**Build:** Release + NATIVE_OPT + LZ4 + RZFP (ZFP 1.0.1)

## Important: T_composite ≠ Total Time

T_composite = process_e2e / 6. The real total time to complete all 330 output files is process_e2e. Previous results using T_composite understated real time by ~6x.

---

## 20GB Small (LZ4+XP, 0.479x, SSD profile, 4GB)

Six groups executed independently (not merged).

### Guest-cold x3 Detail

| Group | Round 1 | Round 2 | Round 3 | median time |
|-------|--------:|--------:|--------:|------------:|
| X random | 4.61s | 4.23s | 4.49s | 4.49s |
| Y random | 2.33s | 2.67s | 2.60s | 2.60s |
| Z random | 2.27s | 2.33s | 2.32s | 2.32s |
| X continuous | 0.29s | 0.32s | 0.32s | 0.32s |
| Y continuous | 0.11s | 0.10s | 0.11s | 0.11s |
| Z continuous | 0.09s | 0.10s | 0.10s | 0.10s |
| **process_e2e** | **9.77s** | **9.81s** | **10.01s** | **9.81s** |
| T_composite | 1.62s | 1.62s | 1.66s | 1.62s |

Group read times (median, per-group, I/O + LZ4 decompress + scatter):

| Group | read_ms | write_ms | notes |
|-------|--------:|---------:|-------|
| X random | 2,751 | 979 | cross-superblock scan, highest cost |
| Y random | 2,263 | 164 | |
| Z random | 2,064 | 303 | |
| X continuous | 196 | 115 | sequential, very fast |
| Y continuous | 62 | 38 | |
| Z continuous | 61 | 38 | |

### 20GB Time Decomposition (median)

```
X random                                 4.49s   ████████████████████████████████████████  46%
Y random                       2.60s   ████████████████████████                  27%
Z random                    2.32s   ████████████████████                        24%
continuous (3 axes)        0.53s   █████                                          5%
─────────────────────────────────────────────────────────────────────────────────────
I/O + LZ4 decompress        6.9s   ██████████████████████████████████████████████████████████ 70%
write                       1.6s   █████████████                                16%
overhead                    1.3s   ██████████                                   14%
─────────────────────────────────────────────────────────────────────────────────────
process_e2e                 9.8s   ██████████████████████████████████████████████████████████████████████████
T_composite                 1.6s   (÷6)
output floor                0.85s  (纯写理论下限)
```

### 20GB Analysis

- **X random 是最大单项**（4.5s，占 46%）：跨 superblock 扫描，XP sidecar (stride=2) 覆盖 50% slices
- **LZ4 解压极快**，没有 decode 瓶颈
- **总时间仅 ~10s**，已很快，无需激进优化
- **写 1.6s (16%)** — 比例略高于 50GB，但绝对值小

---

## 50GB Big (RZFP, 0.421x, HDD profile, 4GB)

Six groups merged into single read via window cache (RZFP merged round).

### Guest-cold x5 Detail

| Round | process_e2e | merged_read | total_write | T_composite |
|------:|------------:|------------:|------------:|------------:|
| 1 (真冷) | 117.4s | 114.7s | 2.7s | 19.57s |
| 2 | 115.8s | 113.0s | 2.8s | 19.29s |
| 3 | 114.9s | 112.1s | 2.8s | 19.15s |
| 4 | 115.6s | 112.7s | 2.9s | 19.27s |
| 5 | 114.5s | 111.6s | 2.9s | 19.08s |
| **median** | **115.4s** | **112.7s** | **2.8s** | **19.2s** |

Group times are estimated shares of merged_read (RZFP merges all 6 groups into one batch):

| Group | estimated (median) |
|-------|-------------------:|
| X random | 34.5s |
| Y random | 35.1s |
| Z random | 34.8s |
| X continuous | 3.5s |
| Y continuous | 3.5s |
| Z continuous | 3.5s |

Three axes very balanced — window cache benefits all equally.

### 50GB Time Decomposition (median)

```
I/O read 21GB @ ~250 MB/s             ~84s   ████████████████████████████████  73%
ZFP decode 21GB @ ~268 MB/s           ~78s   ██████████████████████████████    68%
  (I/O and decode overlap via window cache)
scatter + other                         ~1s                                       1%
write 330 files                       2.8s   █                                     2%
create files                           5ms                                        0%
─────────────────────────────────────────────────────────────────────────────────────
merged_read (I/O+decode+scatter)     112.7s   ████████████████████████████████████████████████████████████████████████████████  98%
total_write                           2.8s   ██                                                                              2%
─────────────────────────────────────────────────────────────────────────────────────
process_e2e                          115.4s   ████████████████████████████████████████████████████████████████████████████████████████████
T_composite                           19.2s   (÷6)
output floor                           3.7s   (纯写理论下限: 7.48 GB, 2030 MB/s)
```

### 50GB Analysis

- **Decode 占 ~68%**：ZFP C 库解码仅 268 MB/s，是绝对瓶颈
- **I/O 占 ~30%**：21 GB 数据，SSD ~250 MB/s 顺序读
- **Write 仅 2.4%**：输出路径已无优化空间
- **Window cache 使三轴均衡**：所有轴共享 superblock 读取
- **纯写地板 3.7s**：即使 decode 和 I/O 做到零，最快也只到 ~4s

---

## ZFP Codec Benchmark (100K leaves × 64 floats)

```
Encode: 4030 ms  (25.6 MB → 14.0 MB, 0.545x)
Decode: 95.4 ms  (268 MB/s decode throughput)
```
ZFP decode at 268 MB/s × 21 GB payload = ~78s pure decode.
This matches the measured merged_read 112.7s (I/O ~84s overlapped with decode ~78s).

---

## Output Floor Benchmark

```
20GB: 330 files, 4.38 GB → 0.85s (5128 MB/s)
50GB: 330 files, 7.48 GB → 3.69s (2030 MB/s)
```
Writing is <4% of total time. Output optimizations (mmap, writer pipeline) have negligible upside.

---

## Comparison

| | 20GB LZ4+XP | 50GB RZFP |
|---|------------|----------|
| Optimized size | 9.2 GB | 21 GB |
| Storage ratio | 0.479x | 0.421x |
| process_e2e median | **9.8s** | **115.4s** |
| T_composite | 1.6s | 19.2s |
| I/O + decode | 6.9s (70%) | 112.7s (98%) |
| Write | 1.6s (16%) | 2.8s (2%) |
| Main bottleneck | X random cross-SB | ZFP decode 78s (68%) |
| Output floor | 0.85s | 3.69s |
| Status | Already fast | Needs faster codec or AxisPack |

---

## Next Steps

1. **Abandon output path optimizations** — write is <4%
2. **Need faster decode** — ZFP at 268 MB/s is the bottleneck
3. **AxisPack** — convert random cross-SB reads to sequential plane reads
4. **FastMantissa attempted but abandoned** — naive bit-packing slower than optimized ZFP C library
