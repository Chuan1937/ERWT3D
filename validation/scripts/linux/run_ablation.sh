#!/usr/bin/env bash
# Materialize A0/A1 artifacts and run A0/A2/A3 using the same production reader.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
raw= dataset= shape= device= dry_run=0
while (($#)); do
    case "$1" in
        --raw|--dataset|--device) key="${1#--}"; shift; printf -v "$key" '%s' "${1:?$key needs a value}" ;;
        --shape) shift; shape="${1:?--shape needs NX}"; shift; shape+=" ${1:?--shape needs NY}"; shift; shape+=" ${1:?--shape needs NZ}" ;;
        --dry-run) dry_run=1 ;;
        *) echo "[ERROR] unknown argument $1" >&2; exit 2 ;;
    esac; shift
done
[[ -n "$raw" && -n "$dataset" && -n "$shape" && -n "$device" ]] || { echo "[ERROR] required: --raw --dataset --shape --device" >&2; exit 2; }
require_roots; require_binary erwt3d_convert; require_binary erwt3d_paper_bench
read -r nx ny nz <<<"$shape"; root="$ERWT3D_SSD_ROOT/erwt3d_ablation/$dataset"; mkdir -p "$root"
declare -A files
for label in A0_auto A1_force_lz4 A1_force_rzfp; do
    force=auto; [[ "$label" == A1_force_lz4 ]] && force=lz4; [[ "$label" == A1_force_rzfp ]] && force=rzfp
    candidate="$root/$label.erwt3d"; files[$label]="$candidate"
    cmd=("$BUILD_DIR/erwt3d_convert" --input "$raw" --output "$candidate" --nx "$nx" --ny "$ny" --nz "$nz" --threads "${ERWT3D_THREADS:-8}")
    [[ "$force" != auto ]] && cmd+=(--force-format "$force")
    printf '[CONVERT %s] ' "$label"; printf '%q ' "${cmd[@]}"; printf '\n'
    if (( ! dry_run )) && [[ ! -f "$candidate" ]]; then "${cmd[@]}"; fi
done
input="${files[A0_auto]}"
if [[ "$device" == HDD ]]; then
    hdd_root="$ERWT3D_HDD_ROOT/erwt3d_ablation/$dataset"; mkdir -p "$hdd_root"
    for label in "${!files[@]}"; do ((dry_run)) || copy_and_verify "${files[$label]}" "$hdd_root/$label.erwt3d"; files[$label]="$hdd_root/$label.erwt3d"; done
    input="${files[A0_auto]}"
fi
run_variant() {
    local label="$1" file="$2"; shift 2; local extra=("$@")
    for pattern in random continuous; do for axis in x y z; do for repeat in $(seq 1 "${ERWT3D_REPETITIONS:-5}"); do
        args=(--input "$file" --dataset "$dataset" --device "$device" --method "$label" --axis "$axis" --pattern "$pattern" --run "$repeat" "${extra[@]}")
        ((dry_run)) && args+=(--dry-run)
        "$SCRIPT_DIR/run_benchmark.sh" "${args[@]}"
    done; done; done
}
run_variant A0_auto "$input"
run_variant A1_force_lz4 "${files[A1_force_lz4]}"
run_variant A1_force_rzfp "${files[A1_force_rzfp]}"
run_variant A2_no_access_planner "$input" --extra --disable-access-planner
run_variant A3_generic_device "$input" --extra --disable-device-scheduling
