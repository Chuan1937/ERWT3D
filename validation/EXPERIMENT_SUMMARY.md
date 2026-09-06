# ERWT3D Paper Benchmark — Experiment Summary

## 1. Tested Commit

| Field | Value |
|-------|-------|
| Repository | |
| Branch | `paper-benchmark` |
| Commit SHA | |
| Dirty | |
| Date | |

## 2. Environment

| Component | Details |
|-----------|---------|
| CPU | |
| RAM | |
| HDD | |
| SSD | |
| OS | |
| Compiler | |
| CMake | |
| ZFP | |
| LZ4 | |
| Threads | |

## 3. Dataset Manifest

| Dataset | Type | Dimensions | Raw Size | SHA256 |
|---------|------|-----------|----------|--------|
| A (velocity) | model | | | |
| B (seismic) | seismic | | | |
| C (wavefield) | wavefield | | | |
| D (random) | control | | | |

## 4. Experiments Completed

| # | Experiment | Status | Notes |
|---|-----------|--------|-------|
| 1 | Compression & Storage | PENDING | |
| 2 | Write Performance | PENDING | |
| 3 | Reconstruction Accuracy | PENDING | |
| 4 | Random Slice Access | PENDING | |
| 5 | Continuous Slice Access | PENDING | |
| 6 | HDD vs SSD | PENDING | |
| 7 | Scalability | PENDING | |
| 8 | Data Type Generalization | PENDING | |
| 9 | Ablation Study | PENDING | |
| 10 | Internal Mechanism Stats | PENDING | |
| 11 | Error Threshold Sensitivity | PENDING | |
| 12 | Geophysical Fidelity | PENDING | |

## 5. Main Numerical Results

### Compression Ratio

| Dataset | Method | CR | SR | Encode Throughput |
|---------|--------|----|----|-------------------|
| A | Raw | 1.0x | 1.0x | — |
| A | LZ4 | | | |
| A | ZFP | | | |
| A | HDF5+gzip | | | |
| A | ERWT3D | | | |

### Reconstruction Error

| Dataset | Method | RMSE | NRMSE | Max Rel Error |
|---------|--------|------|-------|---------------|
| A | ZFP | | | |
| A | ERWT3D | | | |

### Random Access (Cold)

| Method | X mean | X p95 | Y mean | Y p95 | Z mean | Z p95 | Axis Imbalance |
|--------|--------|-------|--------|-------|--------|-------|----------------|
| Raw | | | | | | | |
| LZ4 | | | | | | | |
| ZFP | | | | | | | |
| HDF5 | | | | | | | |
| ERWT3D | | | | | | | |

### Continuous Access (Cold)

| Method | X | Y | Z | Avg | Throughput |
|--------|---|---|---|-----|------------|
| ERWT3D | | | | | |

## 6. Unexpected Observations

- (none yet)

## 7. Potential Bugs

- (none yet)

## 8. Paper-Ready Conclusions

(Awaiting experimental data. Do not pre-write conclusions.)

---

*Generated automatically by ERWT3D benchmark suite.*
*Last updated: (auto-fill)*
