#!/usr/bin/env bash
# Run one Raw/HDF5 workload under the same workload/output protocol as ERWT3D.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
method= input= dataset= device= axis= pattern= run= shape= dry_run=0
while (($#)); do
    case "$1" in
        --method|--input|--dataset|--device|--axis|--pattern|--run) key="${1#--}"; shift; printf -v "$key" '%s' "${1:?$key needs a value}" ;;
        --shape) shift; shape="${1:?--shape needs NX}"; shift; shape+=" ${1:?--shape needs NY}"; shift; shape+=" ${1:?--shape needs NZ}" ;;
        --dry-run) dry_run=1 ;;
        *) echo "[ERROR] unknown argument $1" >&2; exit 2 ;;
    esac; shift
done
[[ -n "$method" && -n "$input" && -n "$dataset" && -n "$device" && -n "$axis" && -n "$pattern" && -n "$run" && -n "$shape" ]] || { echo "[ERROR] missing baseline arguments" >&2; exit 2; }
require_roots; [[ -f "$input" ]] || { echo "[ERROR] missing input $input" >&2; exit 2; }
workload="$VALIDATION_DIR/workloads/$dataset/${pattern}_${axis}.txt"; [[ -f "$workload" ]] || { echo "[ERROR] missing workload $workload" >&2; exit 2; }
id="$(date -u +%Y%m%dT%H%M%SZ)_${device}_${method}_${dataset}_${axis}_${pattern}_run$(printf '%02d' "$run")"
result="$ERWT3D_RESULT_ROOT/raw_results/${pattern}_read/$id.json"; output="$ERWT3D_RESULT_ROOT/slice_outputs/$id"; stdout="$ERWT3D_RESULT_ROOT/logs/$id.stdout.log"; stderr="$ERWT3D_RESULT_ROOT/logs/$id.stderr.log"
cmd=(python3 "$VALIDATION_DIR/scripts/baseline_paper_bench.py" --method "$method" --input "$input" --shape $shape --axis "$axis" --pattern "$pattern" --positions-file "$workload" --output-dir "$output" --metrics-json "$result")
if ((dry_run)); then printf '[COMMAND] '; printf '%q ' "${cmd[@]}"; printf '\n'; exit 0; fi
"$SCRIPT_DIR/drop_caches.sh"
set +e; "${cmd[@]}" >"$stdout" 2>"$stderr"; code=$?; set -e
if ((code)); then write_failed_json "$result" "$code" "baseline benchmark failed; inspect $stderr"; exit "$code"; fi
python3 - "$result" "$id" "$dataset" "$device" "$run" "$(git -C "$PROJECT_ROOT" rev-parse HEAD)" <<'PY'
import json, pathlib, sys
p=pathlib.Path(sys.argv[1]); d=json.loads(p.read_text()); d.update({"run_id":sys.argv[2],"dataset":sys.argv[3],"device":sys.argv[4],"run_number":int(sys.argv[5]),"git_commit":sys.argv[6],"cache_mode":"cold_os_page_cache"}); p.write_text(json.dumps(d,indent=2)+"\n")
PY
