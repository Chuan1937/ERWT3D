# ERWT3D Paper Benchmark Suite

## Purpose

Complete, reproducible benchmark suite for ERWT3D journal paper (Applied Geophysics).
Validates: storage efficiency, multi-axis access, device adaptability, geophysical fidelity.

## Branch

`paper-benchmark` — all experiment code and results live here.

## Quick Start

```bash
# 1. Build ERWT3D (Release)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DERWT3D_ENABLE_RZFP=ON \
      -DCMAKE_PREFIX_PATH=deps/zfp
cmake --build build -j

# 2. Record environment
cd validation
bash scripts/audit_environment.sh

# 3. Generate workloads (random + continuous coordinates)
python3 scripts/generate_workloads.py

# 4. Run full benchmark suite
bash scripts/run_all.sh
```

## Directory Structure

```
validation/
├── README.md                    # This file
├── environment/                 # Server/device environment JSON
│   ├── server.json
│   ├── hdd.json
│   └── ssd.json
├── datasets/                    # Dataset manifest + checksums
│   ├── manifest.csv
│   └── checksums.sha256
├── configs/                     # Method configurations
│   ├── erwt3d.json
│   ├── zfp.json
│   ├── lz4.json
│   └── hdf5.json
├── workloads/                   # Fixed coordinates for reproducibility
│   ├── random_x.txt
│   ├── random_y.txt
│   ├── random_z.txt
│   ├── continuous_x.txt
│   ├── continuous_y.txt
│   └── continuous_z.txt
├── raw_results/                 # Raw per-run results (never delete)
│   ├── compression/
│   ├── accuracy/
│   ├── random_read/
│   ├── continuous_read/
│   ├── ablation/
│   └── forward_modeling/
├── logs/                        # stdout/stderr logs
├── summaries/                   # Aggregated CSV tables
│   ├── compression.csv
│   ├── accuracy.csv
│   ├── read_random.csv
│   ├── read_continuous.csv
│   ├── ablation.csv
│   └── geophysical_fidelity.csv
├── figures/                     # Paper-ready figures
├── scripts/                     # Automation scripts
│   ├── audit_environment.sh
│   ├── generate_workloads.py
│   ├── run_benchmark.sh
│   ├── run_all.sh
│   ├── drop_caches.sh
│   ├── collect_results.py
│   └── utils.sh
└── EXPERIMENT_SUMMARY.md        # Final summary
```

## Experiment Priority

| Priority | Experiment | Status |
|----------|-----------|--------|
| P0-1 | 20GB/50GB HDD+SSD full benchmark | pending |
| P0-2 | X/Y/Z random + continuous | pending |
| P0-3 | Baselines: Raw/LZ4/ZFP/HDF5/ERWT3D | pending |
| P0-4 | Reconstruction error | pending |
| P0-5 | 3+ geophysical datasets + random control | pending |
| P0-6 | Ablation study | pending |
| P0-7 | Forward-modeling fidelity | pending |
| P1 | cold/warm, scalability, error-threshold, breakdown | pending |
| P2 | 100GB+, GPU, MPI, cloud | deferred |

## Rules

- Single commit for all formal results
- 5 repetitions per performance experiment
- 100 fixed random slices per axis
- Cold test: drop_caches before each run
- Save every raw latency (no cherry-picking)
- Anomalous results preserved with explanation
- CV > 5%: mark UNSTABLE, re-test

## Run ID Format

```
YYYYMMDD_DEVICE_METHOD_DATASET_SIZE_AXIS_PATTERN_RUNNN
```

Example: `20260905_SSD_ERWT3D_DatasetB_20GB_X_random_run03`
