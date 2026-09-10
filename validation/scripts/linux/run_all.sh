#!/usr/bin/env bash
# Linux formal protocol. It never runs on macOS and never guesses storage paths.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
only=all; dataset=; device=; dry_run=0; config=
while (($#)); do
    case "$1" in
        --only) shift; only="${1:?--only needs a value}" ;;
        --dataset) shift; dataset="${1:?--dataset needs a value}" ;;
        --device) shift; device="${1:?--device needs a value}" ;;
        --config) shift; config="${1:?--config needs a value}" ;;
        --dry-run) dry_run=1 ;;
        *) echo "Usage: $0 [--config linux.json] [--dry-run] [--only storage|accuracy|access|ablation|fidelity|all] [--dataset ID] [--device SSD|HDD]" >&2; exit 2 ;;
    esac
    shift
done
if [[ -n "$config" ]]; then
    eval "$(python3 - "$config" <<'PY'
import json, shlex, sys
cfg=json.load(open(sys.argv[1]))
for src, dst in (("dataset_root","ERWT3D_DATASET_ROOT"),("ssd_root","ERWT3D_SSD_ROOT"),("hdd_root","ERWT3D_HDD_ROOT"),("result_root","ERWT3D_RESULT_ROOT"),("build_dir","ERWT3D_BUILD_DIR"),("threads","ERWT3D_THREADS")):
    if src in cfg: print(f'export {dst}={shlex.quote(str(cfg[src]))}')
PY
)"
fi
source "$SCRIPT_DIR/common.sh"
require_roots
require_binary erwt3d_convert; require_binary erwt3d_paper_bench
case "$only" in all|storage|accuracy|access|ablation|fidelity) ;; *) echo "[ERROR] invalid --only: $only" >&2; exit 2 ;; esac
[[ -f "$ERWT3D_DATASET_ROOT/manifest.json" ]] || { echo "[ERROR] expected $ERWT3D_DATASET_ROOT/manifest.json (copy and complete validation/datasets/manifest.template.json)" >&2; exit 2; }
"$SCRIPT_DIR/audit_environment.sh"

mapfile -t ids < <(python3 - "$ERWT3D_DATASET_ROOT/manifest.json" "$dataset" <<'PY'
import json, sys
items=json.load(open(sys.argv[1])).get("datasets", [])
requested=sys.argv[2]
for item in items:
    if not requested or item["dataset_id"] == requested: print(item["dataset_id"])
PY
)
[[ ${#ids[@]} -gt 0 ]] || { echo "[ERROR] requested dataset not in manifest" >&2; exit 2; }
devices=(SSD HDD); [[ -n "$device" ]] && devices=("$device")
for id in "${ids[@]}"; do
    mapfile -t info < <(python3 - "$ERWT3D_DATASET_ROOT/manifest.json" "$id" <<'PY'
import json, sys
for x in json.load(open(sys.argv[1]))["datasets"]:
    if x["dataset_id"] == sys.argv[2]: print(x["raw_file"]); print(" ".join(map(str,x["shape"]))); print(x.get("raw_sha256","")); break
PY
)
    raw="$ERWT3D_DATASET_ROOT/${info[0]}"; read -r nx ny nz <<<"${info[1]}"
    [[ -f "$raw" ]] || { echo "[ERROR] missing raw input $raw" >&2; exit 2; }
    [[ -n "${info[2]}" && "${info[2]}" != REPLACE_AFTER_PREPARATION ]] && [[ "$(sha256_file "$raw")" == "${info[2]}" ]] || [[ "${info[2]}" == REPLACE_AFTER_PREPARATION ]] || { echo "[ERROR] raw SHA256 mismatch: $raw" >&2; exit 2; }
    [[ -d "$VALIDATION_DIR/workloads/$id" ]] || { echo "[ERROR] generate workloads for $id before formal execution" >&2; exit 2; }
    ssd_file="$ERWT3D_SSD_ROOT/erwt3d/$id.erwt3d"; hdd_file="$ERWT3D_HDD_ROOT/erwt3d/$id.erwt3d"
    if [[ "$only" == all || "$only" == storage || "$only" == access || "$only" == accuracy || "$only" == ablation ]]; then
        if [[ ! -f "$ssd_file" ]]; then
            mkdir -p "$(dirname "$ssd_file")"
            cmd=("$BUILD_DIR/erwt3d_convert" --input "$raw" --output "$ssd_file" --nx "$nx" --ny "$ny" --nz "$nz" --threads "${ERWT3D_THREADS:-8}")
            printf '[CONVERT] '; printf '%q ' "${cmd[@]}"; printf '\n'
            ((dry_run)) || "${cmd[@]}"
        fi
        [[ -f "$ssd_file" || $dry_run -eq 1 ]] && { ((dry_run)) || copy_and_verify "$ssd_file" "$hdd_file"; }
    fi
    if [[ "$only" == all || "$only" == accuracy ]]; then
        restored="$ERWT3D_RESULT_ROOT/accuracy/$id.restored.raw"; result="$ERWT3D_RESULT_ROOT/raw_results/accuracy/${id}_ERWT3D.json"
        cmd=("$BUILD_DIR/erwt3d_convert" --input "$ssd_file" --output "$restored" --to-raw --threads "${ERWT3D_THREADS:-8}")
        printf '[ACCURACY RESTORE] '; printf '%q ' "${cmd[@]}"; printf '\n'
        if (( ! dry_run )); then
            "${cmd[@]}"
            quantile_args=(--quantiles exact)
            if (( $(stat -c%s "$raw") >= 2 * 1024 * 1024 * 1024 )); then
                quantile_args=(--quantiles sample --sample-size 10000000 --seed 42)
            fi
            python3 "$VALIDATION_DIR/scripts/stream_accuracy.py" --raw "$raw" --restored "$restored" --output "$result" "${quantile_args[@]}"
        fi
    fi
    if [[ "$only" == all || "$only" == storage ]]; then
        storage_dir="$ERWT3D_SSD_ROOT/storage_baselines/$id"
        storage_metrics="$ERWT3D_RESULT_ROOT/raw_results/storage/$id"
        if (( ! dry_run )); then
            mkdir -p "$storage_dir"
            raw_storage="$storage_dir/raw.raw"
            [[ -f "$raw_storage" ]] || copy_and_verify "$raw" "$raw_storage"
            python3 "$VALIDATION_DIR/scripts/prepare/prepare_baselines.py" --raw "$raw_storage" --shape "$nx" "$ny" "$nz" --dataset "$id" --output-dir "$storage_dir" --metrics-dir "$storage_metrics"
            python3 - "$storage_metrics/erwt3d.json" "$id" "$raw" "$ssd_file" <<'PY'
import json, pathlib, sys
raw, optimized = pathlib.Path(sys.argv[3]), pathlib.Path(sys.argv[4])
pathlib.Path(sys.argv[1]).parent.mkdir(parents=True, exist_ok=True)
pathlib.Path(sys.argv[1]).write_text(json.dumps({
  "status":"SUCCESS", "experiment":"storage", "dataset":sys.argv[2], "method":"ERWT3D",
  "raw_size_bytes":raw.stat().st_size, "compressed_size_bytes":optimized.stat().st_size,
  "compression_ratio":raw.stat().st_size / optimized.stat().st_size,
  "encode_time_ms":None, "decode_time_ms":None,
  "timing_note":"The storage artifact was converted by the formal conversion phase; its timing is recorded in the conversion log."
}, indent=2) + "\n")
PY
        else
            echo "[STORAGE] Raw/LZ4/ZFP/HDF5/ERWT3D records for $id"
        fi
    fi
    if [[ "$only" == all || "$only" == access ]]; then
        for dev in "${devices[@]}"; do
            [[ "$dev" == SSD || "$dev" == HDD ]] || { echo "[ERROR] invalid device $dev" >&2; exit 2; }
            input="$ssd_file"; [[ "$dev" == HDD ]] && input="$hdd_file"
            for pattern in random continuous; do for axis in x y z; do
                for repeat in $(seq 1 "${ERWT3D_REPETITIONS:-5}"); do
                    args=(--input "$input" --dataset "$id" --device "$dev" --axis "$axis" --pattern "$pattern" --run "$repeat")
                    ((dry_run)) && args+=(--dry-run)
                    "$SCRIPT_DIR/run_benchmark.sh" "${args[@]}"
                done
            done; done
        done
    fi
    if [[ "$only" == all || "$only" == access ]]; then
        require_binary erwt3d_convert
        ssd_baseline="$ERWT3D_SSD_ROOT/baselines/$id"; hdd_baseline="$ERWT3D_HDD_ROOT/baselines/$id"
        raw_ssd="$ssd_baseline/raw.raw"; raw_hdd="$hdd_baseline/raw.raw"
        if (( ! dry_run )); then
            [[ -f "$raw_ssd" ]] || copy_and_verify "$raw" "$raw_ssd"
            [[ -f "$ssd_baseline/hdf5_raw.h5" && -f "$ssd_baseline/hdf5_gzip.h5" ]] || python3 "$VALIDATION_DIR/scripts/prepare/prepare_hdf5_baselines.py" --raw "$raw_ssd" --shape "$nx" "$ny" "$nz" --output-dir "$ssd_baseline"
            copy_and_verify "$raw_ssd" "$raw_hdd"
            copy_and_verify "$ssd_baseline/hdf5_raw.h5" "$hdd_baseline/hdf5_raw.h5"
            copy_and_verify "$ssd_baseline/hdf5_gzip.h5" "$hdd_baseline/hdf5_gzip.h5"
        fi
        for dev in "${devices[@]}"; do
            base="$ssd_baseline"; [[ "$dev" == HDD ]] && base="$hdd_baseline"
            for method in raw hdf5_raw hdf5_gzip; do
                input="$base/raw.raw"; [[ "$method" != raw ]] && input="$base/$method.h5"
                for pattern in random continuous; do for axis in x y z; do for repeat in $(seq 1 "${ERWT3D_REPETITIONS:-5}"); do
                    args=(--method "$method" --input "$input" --shape "$nx" "$ny" "$nz" --dataset "$id" --device "$dev" --axis "$axis" --pattern "$pattern" --run "$repeat")
                    ((dry_run)) && args+=(--dry-run)
                    "$SCRIPT_DIR/run_baseline.sh" "${args[@]}"
                done; done; done
            done
        done
    fi
    if [[ "$only" == all || "$only" == ablation ]]; then
        for dev in "${devices[@]}"; do
            args=(--raw "$raw" --dataset "$id" --shape "$nx" "$ny" "$nz" --device "$dev")
            ((dry_run)) && args+=(--dry-run)
            "$SCRIPT_DIR/run_ablation.sh" "${args[@]}"
        done
    fi
done
if [[ "$only" == fidelity ]]; then
    : "${ERWT3D_ORIGINAL_MODEL:?Set ERWT3D_ORIGINAL_MODEL for fidelity}"
    : "${ERWT3D_RECONSTRUCTED_MODEL:?Set ERWT3D_RECONSTRUCTED_MODEL for fidelity}"
    : "${ERWT3D_ORIGINAL_GATHER:?Set ERWT3D_ORIGINAL_GATHER for fidelity}"
    : "${ERWT3D_RECONSTRUCTED_GATHER:?Set ERWT3D_RECONSTRUCTED_GATHER for fidelity}"
    : "${ERWT3D_FIDELITY_NREC:?Set ERWT3D_FIDELITY_NREC for fidelity}"
    : "${ERWT3D_FIDELITY_NT:?Set ERWT3D_FIDELITY_NT for fidelity}"
    ((dry_run)) || python3 "$VALIDATION_DIR/scripts/run_forward_fidelity.py" --original-model "$ERWT3D_ORIGINAL_MODEL" --reconstructed-model "$ERWT3D_RECONSTRUCTED_MODEL" --original-gather "$ERWT3D_ORIGINAL_GATHER" --reconstructed-gather "$ERWT3D_RECONSTRUCTED_GATHER" --nrec "$ERWT3D_FIDELITY_NREC" --nt "$ERWT3D_FIDELITY_NT" --output "$ERWT3D_RESULT_ROOT/raw_results/forward_modeling/fidelity.json"
fi
((dry_run)) || python3 "$VALIDATION_DIR/scripts/collect_results.py"
