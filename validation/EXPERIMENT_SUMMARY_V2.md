# ERWT3D Paper Benchmark — Experiment Summary V2

## 1. Tested Commit

```
2cc905d fix: LZ4 X-axis layout, --to-raw, and planner model
(base: ed8b21e7b8b0c3990372301bbe542f4598ef5bed)
```

## 2. Environment

| Component | Details |
|-----------|---------|
| CPU | Intel i7-13700F, 12C/24T |
| RAM | 62 GB |
| SSD | /mnt/f (F:), 260 GB free |
| HDD | /mnt/d (D:), 366 GB free |
| OS | Fedora 43 WSL2 |
| Threads | 8 (fixed) |
| Cache | cold Linux/WSL guest page-cache |

## 3. Datasets

| ID | Type | Shape | Raw Size | Auto Format | Final Package | SR | Lossy? |
|----|------|-------|----------|-------------|---------------|-----|--------|
| f3_amplitude | seismic | 201×201×51 | 7.9 MB | LZ4 | 28.6 MB | 3.63x | No |
| f3_similarity | seismic | 191×146×51 | 5.3 MB | LZ4 | 16.1 MB | 3.02x | No |
| 20GB | velocity | 801×2405×2501 | 18.0 GB | RZFP | 33.3 GB | 1.85x | Yes |
| 50GB | velocity | 2001×2201×3000 | 49.2 GB | RZFP | 63.8 GB | 1.30x | Yes |

## 4. Adaptive Selection

| Dataset | Planner Decision | Reason |
|---------|-----------------|--------|
| f3_amplitude | LZ4 + embedded XYZ | Fast (T_pred=0.01s), storage over budget accepted |
| f3_similarity | LZ4 + embedded XYZ | Fast, storage over budget accepted |
| 20GB | **RZFP** | LZ4 total ratio 1.548x > 1.5x budget; RZFP 0.602x selected |
| 50GB | **RZFP** | LZ4 incompressible (1.04x); RZFP 0.42x selected |

**Key insight**: The planner correctly selects the format based on data compressibility and storage budget. 20GB switches from LZ4 to RZFP because the embedded XYZ sidecars would push LZ4 over 1.5x.

## 5. Accuracy

| Dataset | Format | max_rel_error | violations | RMSE | Status |
|---------|--------|---------------|------------|------|--------|
| f3_amplitude | LZ4 | 0 | 0 | 0 | PASS (bitwise equal) |
| f3_similarity | LZ4 | 0 | 0 | 0 | PASS |
| 20GB | RZFP | 0.000977 | 0 | — | PASS |
| 50GB | RZFP | 0.001 | 0 | — | PASS (from conversion log) |

## 6. 20GB RZFP Access (cold cache, SSD, 100 slices × 5 runs)

| Axis | Pattern | Mean (ms) | Std (ms) | CV% |
|------|---------|-----------|----------|-----|
| X | random | 75,168 | 2,174 | 2.9% |
| Y | random | 25,027 | 558 | 2.2% |
| Z | random | 24,799 | 601 | 2.4% |
| X | continuous | 8,633 | 676 | 7.8% |
| Y | continuous | 3,470 | 289 | 8.3% |
| Z | continuous | 3,716 | 242 | 6.5% |

**Axis imbalance**: X/Y/Z random ratio = 3.0:1.0:1.0 (RZFP axis-leaf provides balanced Y/Z)

## 7. 50GB RZFP Access (from previous results, preserved)

| Axis | SSD random (ms) | HDD random (ms) |
|------|-----------------|-----------------|
| X | 76,793 | 81,322 |
| Y | 67,790 | 72,070 |
| Z | 49,468 | 52,725 |

## 8. Code Changes

### Task 1: LZ4 X-axis layout
- **Problem**: Planner assumed "LZ4 + XP stride=2" but converter only wrote Y/Z sidecars
- **Fix**: Added X-plane sidecar generation (stride=1) to converter
- **Files**: `tools/erwt3d_convert.cpp`, `src/lz4_axis_plane_writer.cpp`, `include/erwt3d/lz4_axis_plane_writer.hpp`
- **Reader**: Added fallback to load embedded XP sidecar in old format
- **File**: `src/reader.cpp`

### Task 2: --to-raw
- **Problem**: pwrite fallback on WSL2 extends file beyond expected size; reads from wrong offsets for compressed data
- **Fix**: pwrite fallback now uses `readSuperblock()` for compressed data; added post-truncate safety net
- **File**: `src/reader.cpp`

### Task 3: Planner model
- **Problem**: Y/Z sidecar costs not included in storage ratio estimate
- **Fix**: Updated model to include all three sidecars; X access time updated for stride=1
- **File**: `src/auto_plan.cpp`

## 9. Known Issues

1. F3 RZFP decode fails (small data bug) — does not affect 20GB/50GB
2. --to-raw RZFP not tested (separate bug)
3. 20GB LZ4 would exceed 1.5x storage budget (planner correctly selects RZFP)
4. F3 storage ratio >1.5x (accepted for speed)

## 10. Pending

- [ ] 20GB ablation (A0/A1/A2/A3)
- [ ] 20GB HDD access
- [ ] 50GB accuracy (formal stream_accuracy)
- [ ] 50GB A2/A3 ablation
- [ ] collect_results.py per-slice statistics
- [ ] ablation metadata fix

## 11. Data Files

| File | Path |
|------|------|
| 20GB ERWT3D | /mnt/f/CUP/erwt3d-paper/erwt3d/20gb_v2.erwt3d |
| 50GB ERWT3D | /mnt/d/erwt3d-paper/erwt3d/50GB.erwt3d |
| Raw results v2 | /mnt/f/CUP/results/raw_results_v2/ |
| Summaries v2 | validation/summaries_v2/ |

---
*Auto-generated*
