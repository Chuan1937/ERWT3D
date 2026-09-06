#!/usr/bin/env bash
# Run one ERWT3D axis/pattern workload and preserve an explicit SUCCESS/FAILED JSON.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

usage() { echo "Usage: $0 --input FILE --dataset ID --device SSD|HDD --axis x|y|z --pattern random|continuous --run N [--dry-run] [--extra ARG]" >&2; }
input= dataset= device= axis= pattern= run= dry_run=0
extra=()
while (($#)); do
    case "$1" in
        --input|--dataset|--device|--axis|--pattern|--run) key="${1#--}"; shift; [[ $# -gt 0 ]] || { usage; exit 2; }; printf -v "$key" '%s' "$1" ;;
        --extra) shift; [[ $# -gt 0 ]] || { usage; exit 2; }; extra+=("$1") ;;
        --dry-run) dry_run=1 ;;
        *) usage; exit 2 ;;
    esac
    shift
done
[[ -n "$input" && -n "$dataset" && -n "$device" && -n "$axis" && -n "$pattern" && -n "$run" ]] || { usage; exit 2; }
[[ "$device" == SSD || "$device" == HDD ]] || { echo "[ERROR] device must be SSD or HDD" >&2; exit 2; }
require_roots; require_binary erwt3d_paper_bench
[[ -f "$input" ]] || { echo "[ERROR] input missing: $input" >&2; exit 2; }
workload="$VALIDATION_DIR/workloads/$dataset/${pattern}_${axis}.txt"
[[ -f "$workload" ]] || { echo "[ERROR] workload missing: $workload" >&2; exit 2; }
run_id="$(date -u +%Y%m%dT%H%M%SZ)_${device}_ERWT3D_${dataset}_${axis}_${pattern}_run$(printf '%02d' "$run")"
category="${pattern}_read"
result="$ERWT3D_RESULT_ROOT/raw_results/$category/$run_id.json"
stdout="$ERWT3D_RESULT_ROOT/logs/$run_id.stdout.log"; stderr="$ERWT3D_RESULT_ROOT/logs/$run_id.stderr.log"
output_dir="$ERWT3D_RESULT_ROOT/slice_outputs/$run_id"
mkdir -p "$(dirname "$result")" "$(dirname "$stdout")"
cmd=("$BUILD_DIR/erwt3d_paper_bench" --input "$input" --output-dir "$output_dir" --positions-file "$workload" --axis "$axis" --pattern "$pattern" --metrics-json "$result" --threads "${ERWT3D_THREADS:-8}" --io-profile "${ERWT3D_IO_PROFILE:-auto}" "${extra[@]}")
printf '[COMMAND] '; printf '%q ' "${cmd[@]}"; printf '\n'
if ((dry_run)); then exit 0; fi
"$SCRIPT_DIR/drop_caches.sh"
set +e
"${cmd[@]}" >"$stdout" 2>"$stderr"; code=$?
set -e
if ((code != 0)); then
    write_failed_json "$result" "$code" "erwt3d_paper_bench failed; inspect $stderr"
    echo "[FAILED] $run_id" >&2
    exit "$code"
fi
python3 - "$result" "$run_id" "$dataset" "$device" "$run" "$(git -C "$PROJECT_ROOT" rev-parse HEAD)" <<'PY'
import json, pathlib, sys
p=pathlib.Path(sys.argv[1]); data=json.loads(p.read_text())
data.update({"run_id":sys.argv[2], "dataset":sys.argv[3], "device":sys.argv[4], "run_number":int(sys.argv[5]), "git_commit":sys.argv[6], "cache_mode":"cold_os_page_cache"})
p.write_text(json.dumps(data, indent=2) + "\n")
PY
echo "[OK] $result"
