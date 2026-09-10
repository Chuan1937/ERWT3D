# ERWT3D Paper Benchmark — Final Summary

## 1. Tested Commit

```
b07a83b fix: embedded XP sidecar baseOffset for correct chunk reads
```

## 2. Environment

| Component | Details |
|-----------|---------|
| CPU | Intel i7-13700F, 12C/24T |
| RAM | 62 GB |
| SSD | /mnt/f (F:) |
| HDD | /mnt/d (D:) |
| OS | Fedora 43 WSL2 |
| Threads | 8 (fixed) |
| Cache | cold Linux/WSL guest page-cache |

## 3. Datasets & Storage

| ID | Shape | Raw | Auto Format | Package | SR | Lossy |
|----|-------|-----|-------------|---------|-----|-------|
| f3_amplitude | 201×201×51 | 7.9 MB | LZ4+XYZ | 28.6 MB | 3.63x | No |
| f3_similarity | 191×146×51 | 5.3 MB | LZ4+XYZ | 16.1 MB | 3.02x | No |
| **20GB** | 801×2405×2501 | 18.0 GB | **LZ4+XYZ** | 19.5 GB | **1.012x** | **No** |

## 4. Adaptive Selection

| Dataset | Decision | Reason |
|---------|----------|--------|
| f3_amplitude | LZ4+XYZ | Fast (T_pred=0.01s) |
| f3_similarity | LZ4+XYZ | Fast |
| **20GB** | **LZ4+XYZ** | LZ4 1.47x vs RZFP 1.87x; LZ4 faster and smaller |

**Key**: After fixing the RZFP planner to account for3x axis-leaf overhead, LZ4 is now correctly seen as both faster and smaller for20GB.

## 5. Accuracy

| Dataset | Format | max_rel_error | violations | Status |
|---------|--------|---------------|------------|--------|
| f3_amplitude | LZ4 | 0 | 0 | bitwise equal |
| f3_similarity | LZ4 | 0 | 0 | bitwise equal |
| 20GB | LZ4 | 0 | 0 | **lossless** |

## 6. 20GB LZ4 Access (cold cache, 100 slices × 5 runs)

| Device | Axis | Random (ms) | Continuous (ms) |
|--------|------|-------------|-----------------|
| SSD | X | 25,917 | 2,565 |
| SSD | Y | 10,157 | 1,145 |
| SSD | Z | 8,364 | 870 |
| HDD | X | 19,215 | 2,069 |
| HDD | Y | 7,864 | 793 |
| HDD | Z | 6,099 | 632 |

**Axis balance**: X:Y:Z ≈ 3:1.2:1.0 (SSD random)

## 7. Code Changes

| Change | Impact |
|--------|--------|
| RZFP planner: 3x axis-leaf overhead | Correct RZFP total ratio estimation |
| LZ4 X sidecar (stride=1) | X random: 385s → 25s |
| XP baseOffset fix | Correct chunk reads for embedded XP |
| --to-raw pwrite fix | WSL2 file extension bug |

## 8. Key Findings

1. **20GB Auto → LZ4**: After correct accounting, LZ4 (1.012x) beats RZFP (1.87x) in both speed and storage
2. **Lossless**: LZ4 provides zero-error reconstruction
3. **Multi-axis**: Embedded XYZ sidecars provide fast access to all three axes
4. **Axis imbalance**: X is3x slower than Z due to XP sidecar overhead vs direct Y/Z sidecar

## 9. Pending

- [ ] efwi3D geophysical fidelity
- [ ] F3 per-slice statistics
- [ ] Formal stream_accuracy.py

## 10. Data Files

| Path | Content |
|------|---------|
| /mnt/f/CUP/erwt3d-paper/erwt3d/20gb_plan_test.erwt3d | 20GB LZ4+XYZ |
| /mnt/d/erwt3d-paper/erwt3d/20gb_lz4.erwt3d | 20GB LZ4 (HDD copy) |
| /mnt/f/CUP/results/raw_results_v2/ | 325 JSON results |
| validation/summaries_v2/ | CSV summaries |

---
*Auto-generated*
