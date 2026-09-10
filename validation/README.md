# ERWT3D paper benchmark protocol

This directory prepares reproducible inputs on macOS and runs frozen formal experiments on Linux. macOS timing is never written as a paper performance result.

## Stage A — local preparation (macOS or Linux)

Build only for functional checks; do not benchmark. Prepare public data without committing binaries:

```bash
python3 validation/scripts/prepare/fetch_public_datasets.py
python3 validation/scripts/prepare/prepare_segy.py \
  --input validation/datasets/cache/f3_amplitude.sgy --dataset-id f3_amplitude \
  --source https://github.com/equinor/segyio-notebooks
python3 validation/scripts/generate_workloads.py \
  --metadata validation/datasets/prepared/f3_amplitude.metadata.json
python3 validation/scripts/prepare/pack_datasets.py
```

`f3_amplitude` and `f3_similarity` originate in Equinor's `segyio-notebooks` Netherlands Offshore F3 data, recorded in [sources.json](datasets/sources.json) as CC-BY-SA. `prepare_segy.py` uses `segyio.tools.cube`, writes C-contiguous float32 X-Y-Z raw data (Z fastest), preserves amplitude values, and writes SHA256/statistics metadata. Original SEG-Y remains in `datasets/cache`; prepared `.raw`, `.sgy`, cache, and bundles are ignored by Git.

Synthetic controls are strictly named `synthetic_smooth_model`, `synthetic_correlated_volume`, `synthetic_wavefield`, and `random_control`; none is real seismic data. The generator rejects competition-scale inputs and writes in X chunks instead of allocating whole-volume meshgrids.

`stream_accuracy.py` evaluates raw/reconstructed files chunkwise. Its NRMSE is `sqrt(sum(error²)/sum(raw²))`; percentile results are marked `exact` only when actually exact. The Linux runner uses exact quantiles below 2 GiB and a deterministic 10,000,000-point sample (seed 42) at or above 2 GiB; RMSE, NRMSE, maximum relative error, and violation counts always remain full-volume calculations.

## Stage B — Linux formal benchmark

Copy `configs/linux.example.json` to a private path and replace every path. Load it into the environment or pass it to the runner. The runner refuses missing roots; it never guesses `/mnt/*` paths.

```bash
cp validation/configs/linux.example.json /secure/linux.json
# Populate DATASET_ROOT/manifest.json from validation/datasets/manifest.template.json.
bash validation/scripts/linux/run_all.sh --config /secure/linux.json --dry-run
bash validation/scripts/linux/run_all.sh --config /secure/linux.json --only storage --dataset f3_amplitude
bash validation/scripts/linux/run_all.sh --config /secure/linux.json --only access --dataset f3_amplitude
```

The Linux protocol audits device paths/mounts/model/ROTA, checks raw checksums, builds or verifies the SSD ERWT3D copy, copies it to HDD and verifies SHA256, then runs every axis/pattern independently. A cold run means **cold OS page-cache** only: cache dropping occurs before each workload. It does not claim a completely cold SSD.

`erwt3d_paper_bench` is the machine-readable ERWT3D access entrypoint. Each invocation accepts exactly one axis and one pattern and writes positions, every slice's end-to-end/read/write latency, read/write totals, bytes, and supported decoder/reorder timings to `--metrics-json`. It uses the production Reader APIs rather than a separate reader implementation. A missing fine-grained LZ4 decoder breakdown is explicitly `null`.

Formal output is append-only by run ID under `raw_results`; failures retain stdout/stderr and a `status: FAILED` JSON. `collect_results.py` excludes failures from statistics and writes `n`, mean, standard deviation, CV, median, p95, p99, and `UNSTABLE` for CV >5%.

## Baselines and ablations

`--only storage` measures Raw, LZ4, HDF5 chunked raw, HDF5 gzip, standard ZFP, and ERWT3D storage artifacts. `prepare/prepare_baselines.py` records actual ZFP reconstruction error over a tolerance sweep; it does not equate a ZFP tolerance to ERWT3D pointwise-relative semantics.

`--only access` measures Raw, HDF5 chunked raw, HDF5 gzip, and ERWT3D using identical axis/pattern/position/output rules. Standalone LZ4/ZFP remain storage baselines because they do not supply a comparable random-slice container.

Ablation definitions for the Linux run are A0 full ERWT3D, A1 separately converted fixed-LZ4/fixed-RZFP artifacts, A2 `--disable-access-planner`, and A3 `--disable-device-scheduling`. A1 must use genuinely distinct converted artifacts; no duplicate configuration is accepted as an ablation result.

`run_forward_fidelity.py` is an integration interface for an external solver (such as efwi3D), comparing original/reconstructed models and gathers. ERWT3D does not reimplement a wave-equation solver.

## Directory roles

`scripts/prepare/` and `generate_workloads.py` are cross-platform preparation tools. `scripts/linux/` is Linux-only formal execution. Legacy scripts in `scripts/` are historical and must not be used for paper timing; new paper access runs use `erwt3d_paper_bench`, not `erwt3d_bench_contest`.
