# ERWT3D Paper Benchmark — Experiment Summary V2

## 1. Tested Commit

```
2cc905d fix: LZ4 X-axis layout, --to-raw, and planner model
(base: ed8b21e7b8b0c3990372301bbe542f4598ef5bed PR#65)
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
| 20GB | 801×2405×2501 | 18.0 GB | RZFP | 33.3 GB | 1.85x | Yes |
| 50GB | 2001×2201×3000 | 49.2 GB | RZFP | 63.8 GB | 1.30x | Yes |

## 4. Adaptive Selection

| Dataset | Decision | Reason |
|---------|----------|--------|
| f3_amplitude | LZ4+XYZ | Fast (T_pred=0.01s), storage over budget accepted |
| f3_similarity | LZ4+XYZ | Fast, storage over budget accepted |
| **20GB** | **RZFP** | LZ4 total ratio 1.548x > 1.5x budget; RZFP 0.602x |
| **50GB** | **RZFP** | LZ4 incompressible (1.04x); RZFP 0.42x |

## 5. Accuracy

| Dataset | Format | max_rel_error | violations | Status |
|---------|--------|---------------|------------|--------|
| f3_amplitude | LZ4 | 0 | 0 | bitwise equal |
| f3_similarity | LZ4 | 0 | 0 | bitwise equal |
| 20GB | RZFP | 0.000977 | 0 | PASS |
| 50GB | RZFP | 0.000810 | 0 | PASS |

## 6. 20GB Access (cold cache, SSD, 100 slices × 5 runs)

| Axis | Random (ms) | Continuous (ms) |
|------|-------------|-----------------|
| X | 75,168 ± 2,174 | 8,633 ± 676 |
| Y | 25,027 ± 558 | 3,470 ± 289 |
| Z | 24,799 ± 601 | 3,716 ± 242 |

**Axis imbalance**: X:Y:Z = 3.0:1.0:1.0

## 7. 20GB Ablation (SSD, random, mean ms)

| Config | X | Y | Z |
|--------|---|---|---|
| A0_auto (RZFP) | 79,578 | 25,934 | 25,141 |
| A1_force_lz4 | 379,961 | 6,390 | 4,992 |
| A1_force_rzfp | 77,607 | 26,355 | 25,921 |
| A2_no_planner | 75,151 | 25,348 | 25,336 |
| A3_generic | 78,060 | 26,707 | 26,057 |

**Key findings**:
- A0 ≈ A1_rzfp ≈ A2 ≈ A3 (RZFP path dominates)
- A1_lz4: Y/Z 4-5x faster but X 5x slower (X scan bottleneck)
- Access planner and device scheduling have small effect on RZFP

## 8. 50GB Access (cold cache, SSD, 100 slices × 5 runs)

| Axis | Random (ms) | Continuous (ms) |
|------|-------------|-----------------|
| X | 80,176 | 9,737 |
| Y | 71,633 | 8,888 |
| Z | 51,125 | 7,050 |

## 9. 50GB Ablation (SSD, random, mean ms)

| Config | X | Y | Z |
|--------|---|---|---|
| A0_auto | 80,176 | 71,633 | 51,125 |
| A2_no_planner | 75,009 | 67,261 | 50,222 |

## 10. HDD vs SSD (20GB RZFP)

| Axis | SSD random | HDD random | SSD/HDD ratio |
|------|------------|------------|---------------|
| X | 75,168 | 76,971 | 0.98 |
| Y | 25,027 | 25,611 | 0.98 |
| Z | 24,799 | 24,853 | 1.00 |

SSD ≈ HDD (RZFP decode dominates, not I/O)

## 11. Code Changes

| Change | File | Impact |
|--------|------|--------|
| LZ4 X sidecar (stride=1) | converter, reader | F3 X: 354s → 0.17s |
| --to-raw pwrite fix | reader | WSL2 file extension bug |
| Planner XYZ model | auto_plan | Correct storage ratio |

## 12. Pending

- [ ] collect_results.py per-slice statistics
- [ ] ablation metadata recovery
- [ ] Formal stream_accuracy.py for all datasets

## 13. Data Files

| Path | Content |
|------|---------|
| /mnt/f/CUP/erwt3d-paper/erwt3d/20gb_v2.erwt3d | 20GB RZFP |
| /mnt/d/erwt3d-paper/erwt3d/50GB.erwt3d | 50GB RZFP |
| /mnt/d/erwt3d-paper/erwt3d/20gb_v2.erwt3d | 20GB RZFP (HDD copy) |
| /mnt/f/CUP/results/raw_results_v2/ | 265 JSON results |
| validation/summaries_v2/ | CSV summaries |

---
*Auto-generated*
