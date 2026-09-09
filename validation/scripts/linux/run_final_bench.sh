#!/usr/bin/env bash
# FINAL benchmark runner with strict metadata schema.
# Usage: run_final_bench.sh --input FILE --dataset ID --format FMT --layout LAY \
#        --configuration CFG --device SSD|HDD --axis x|y|z --pattern random|continuous --run N
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VALIDATION_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
PROJECT_ROOT="$(cd "$VALIDATION_DIR/.." && pwd)"
BUILD_DIR="${ERWT3D_BUILD_DIR:-$PROJECT_ROOT/build}"
RESULT_ROOT="${ERWT3D_RESULT_ROOT:-/mnt/f/CUP/results/raw_results_final}"
ALGORITHM_COMMIT="${ERWT3D_ALGORITHM_COMMIT:-$(git -C "$PROJECT_ROOT" rev-parse HEAD)}"

input= dataset= format= layout= configuration= device= axis= pattern= run=
extra=()
dry_run=0
while (($#)); do
    case "$1" in
        --input)       shift; input="$1" ;;
        --dataset)     shift; dataset="$1" ;;
        --format)      shift; format="$1" ;;
        --layout)      shift; layout="$1" ;;
        --configuration) shift; configuration="$1" ;;
        --device)      shift; device="$1" ;;
        --axis)        shift; axis="$1" ;;
        --pattern)     shift; pattern="$1" ;;
        --run)         shift; run="$1" ;;
        --extra)       shift; extra+=("$1") ;;
        --dry-run)     dry_run=1 ;;
        *) echo "Unknown arg: $1" >&2; exit 2 ;;
    esac
    shift
done

# Validate required fields
for var in input dataset format layout configuration device axis pattern run; do
    if [[ -z "${!var}" ]]; then
        echo "[ERROR] --$var is required" >&2; exit 2
    fi
done
[[ "$device" == SSD || "$device" == HDD ]] || { echo "[ERROR] device must be SSD or HDD" >&2; exit 2; }
[[ -f "$input" ]] || { echo "[ERROR] input missing: $input" >&2; exit 2; }

workload="$VALIDATION_DIR/workloads/$dataset/${pattern}_${axis}.txt"
[[ -f "$workload" ]] || { echo "[ERROR] workload missing: $workload" >&2; exit 2; }

run_id="$(date -u +%Y%m%dT%H%M%SZ)_${dataset}_${configuration}_${device}_${axis}_${pattern}_run$(printf '%02d' "$run")"
result="$RESULT_ROOT/${pattern}_read/${run_id}.json"
stdout_log="$RESULT_ROOT/logs/${run_id}.stdout.log"
stderr_log="$RESULT_ROOT/logs/${run_id}.stderr.log"
output_dir="$RESULT_ROOT/slice_outputs/$run_id"

mkdir -p "$(dirname "$result")" "$(dirname "$stdout_log")" "$output_dir"

cmd=("$BUILD_DIR/erwt3d_paper_bench"
    --input "$input"
    --output-dir "$output_dir"
    --positions-file "$workload"
    --axis "$axis"
    --pattern "$pattern"
    --metrics-json "$result"
    --threads "${ERWT3D_THREADS:-8}"
    --io-profile "${ERWT3D_IO_PROFILE:-auto}"
    "${extra[@]}")

printf '[COMMAND] '; printf '%q ' "${cmd[@]}"; printf '\n'
if ((dry_run)); then exit 0; fi

# Drop caches for cold test
sync
if [[ -w /proc/sys/vm/drop_caches ]]; then
    echo 3 > /proc/sys/vm/drop_caches
elif command -v sudo &>/dev/null && sudo -n true 2>/dev/null; then
    echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
fi

set +e
"${cmd[@]}" >"$stdout_log" 2>"$stderr_log"; code=$?
set -e

if ((code != 0)); then
    python3 - "$result" "$code" "erwt3d_paper_bench failed" <<'PY'
import json, pathlib, sys
pathlib.Path(sys.argv[1]).parent.mkdir(parents=True, exist_ok=True)
pathlib.Path(sys.argv[1]).write_text(json.dumps({
    "status":"FAILED", "exit_code":int(sys.argv[2]), "error":sys.argv[3]
}, indent=2)+"\n")
PY
    echo "[FAILED] $run_id" >&2; exit "$code"
fi

# Inject strict metadata into result JSON
python3 - "$result" <<PYEOF
import json, pathlib, sys, datetime
p = pathlib.Path(sys.argv[1])
data = json.loads(p.read_text())

data.update({
    "benchmark_schema_version": "final-1",
    "status": "SUCCESS",
    "run_id": "${run_id}",
    "git_commit": "${ALGORITHM_COMMIT}",
    "algorithm_commit": "${ALGORITHM_COMMIT}",
    "dataset": "${dataset}",
    "format": "${format}",
    "layout": "${layout}",
    "configuration": "${configuration}",
    "device": "${device}",
    "axis": "${axis}",
    "pattern": "${pattern}",
    "run_number": ${run},
    "threads": ${ERWT3D_THREADS:-8},
    "cache_mode": "cold_linux_guest_page_cache",
    "input_path": "${input}",
    "workload_path": "${workload}",
    "timestamp_utc": datetime.datetime.utcnow().isoformat() + "Z",
})
p.write_text(json.dumps(data, indent=2) + "\n")
PYEOF

echo "[OK] $result"
