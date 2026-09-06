# Local preparation report

## Scope and revision

- Starting revision: `0b48c9c47adca03cff86c8c12893729c0cb7ea75` (`paper-benchmark`)
- Host phase: macOS local preparation only
- Performance results: none. No macOS timing is a paper result.

## Implemented protocol changes

- Removed fixed benchmark-storage assumptions from the new Linux protocol. It requires `ERWT3D_DATASET_ROOT`, `ERWT3D_SSD_ROOT`, `ERWT3D_HDD_ROOT`, and `ERWT3D_RESULT_ROOT`.
- Added `erwt3d_paper_bench`: a single-axis/single-pattern production Reader benchmark with JSON metrics and a latency for every requested slice.
- Replaced sorted random workload generation with preserved sampled order, per-dataset metadata, dimensions, seed, timestamp, and per-file SHA256.
- Added public-dataset fetch, SEG-Y conversion, metadata, and portable bundle tools.
- Added chunked streaming reconstruction accuracy, formal-result aggregation that excludes failures, a forward-modeling integration interface, and a Linux-only orchestration skeleton.
- Corrected the HDF5 chunk description and removed unmeasured ZFP `expected_cr` claims.

## Public data state

The declared sources are `f3_amplitude` and `f3_similarity` from Equinor's `segyio-notebooks`, licensed CC-BY-SA. Their URLs and license are versioned in `datasets/sources.json`.

They have **not been downloaded in this checkout**. Therefore file sizes, cube dimensions, and SHA256 values are intentionally not fabricated. Run `fetch_public_datasets.py`, then `prepare_segy.py`; each prepared metadata JSON records the real source/prepared SHA256, bytes, shape, and statistics.

## Methods and ablations

- Storage: Raw, LZ4, HDF5 chunked raw, HDF5 gzip, standard ZFP sweep, ERWT3D.
- Multi-axis access: Raw, HDF5 chunked, ERWT3D. Standalone LZ4/ZFP are not represented as fair random-access containers.
- A0 full ERWT3D; A1 independently converted forced-codec artifacts; A2 access planner disabled; A3 device-aware scheduling disabled.

## Execution boundaries

Cross-platform preparation: `scripts/prepare/`, `generate_workloads.py`, `generate_datasets.py`, `stream_accuracy.py`, and `run_forward_fidelity.py`.

Linux-only formal execution: `scripts/linux/`. It audits paths/devices, validates checksums, creates and SHA256-verifies physical SSD/HDD copies, drops the OS page cache before each workload, preserves failed-run JSON/logs, and aggregates results.

## Still required on the Linux server

1. Build the frozen commit with RZFP enabled.
2. Transfer the prepared data bundle, fill `manifest.json` from the template, and generate/transfer workloads.
3. Copy and complete `linux.example.json` outside Git.
4. Run `run_all.sh --dry-run`, then formal storage, accuracy, access, and ablation experiments.
5. Integrate efwi3D (or another external solver) before running forward-modeling fidelity.

## Known limitations

- macOS cannot validate this repository's Linux-specific cold-I/O implementation; a syntax-only check was used for the new RZFP benchmark source.
- The production converter currently has a fixed RZFP error policy, so error-bound sensitivity remains disabled until a safe, end-to-end runtime parameter is added and verified.
- A1 requires real forced-LZ4 and forced-RZFP conversion outputs. `erwt3d_convert --force-format lz4|rzfp` selects an actual measured format candidate; it must not be substituted with duplicate reader flags.
