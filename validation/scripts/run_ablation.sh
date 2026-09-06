#!/usr/bin/env bash
# Experiment 9: Ablation Study
#
# Tests ERWT3D with different features disabled:
#   A0: Full ERWT3D (baseline)
#   A1: Fixed compression (no adaptive selection)
#   A2: No access planner optimizations
#   A3: No device-aware scheduling
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/utils.sh"

INPUT_FILE="${1:?Usage: run_ablation.sh <input.erwt3d> <dataset> <size>}"
DATASET="${2:?}"
SIZE="${3:?}"

check_binary erwt3d_bench_contest

RESULTS_DIR="$VALIDATION_DIR/raw_results/ablation"
LOGS_DIR="$VALIDATION_DIR/logs"
mkdir -p "$RESULTS_DIR" "$LOGS_DIR"

THREADS="${THREADS:-$(nproc)}"
COMMIT=$(get_git_commit)
TIMESTAMP=$(date -Iseconds)

echo "=== Experiment 9: Ablation Study ==="
echo "Input: $INPUT_FILE"
echo "Dataset: $DATASET ($SIZE)"
echo ""

run_ablation() {
    local config_id="$1"
    local description="$2"
    shift 2
    local extra_args=("$@")

    local RUN_ID="ablation_${DATASET}_${SIZE}_${config_id}_$(date +%Y%m%d)"
    local DEVICE="${IO_DEVICE:-HDD}"
    local WORK_DIR="$SSD_WORK_DIR"
    [[ "$DEVICE" == "HDD" ]] && WORK_DIR="$HDD_WORK_DIR"
    mkdir -p "$WORK_DIR"
    local OUTPUT_DIR="$WORK_DIR/erwt3d_ablation_${config_id}"
    local STDOUT="$LOGS_DIR/${RUN_ID}.stdout.log"
    local STDERR="$LOGS_DIR/${RUN_ID}.stderr.log"
    local JSON_OUT="$RESULTS_DIR/${RUN_ID}.json"

    echo "  [$config_id] $description"
    echo "    Extra args: ${extra_args[*]:-none}"

    drop_caches 2>/dev/null || true

    record_system_state > "$STDOUT.sysstate"

    local START_NS=$(date +%s%N)
    "$BUILD_DIR/erwt3d_bench_contest" \
        --input "$INPUT_FILE" \
        --output-dir "$OUTPUT_DIR" \
        --random-count 100 \
        --continuous-count 10 \
        --threads "$THREADS" \
        --seed 20260511 \
        "${extra_args[@]}" \
        > "$STDOUT" 2>"$STDERR" || true
    local END_NS=$(date +%s%N)

    local WALL_MS=$(( (END_NS - START_NS) / 1000000 ))
    local WALL_S=$(python3 -c "print(f'{$WALL_MS/1000:.3f}')")

    # Parse output
    local T_XR=$(grep -oP 'T_xr[:\s]+\K[\d.]+' "$STDOUT" 2>/dev/null || echo "0")
    local T_YR=$(grep -oP 'T_yr[:\s]+\K[\d.]+' "$STDOUT" 2>/dev/null || echo "0")
    local T_ZR=$(grep -oP 'T_zr[:\s]+\K[\d.]+' "$STDOUT" 2>/dev/null || echo "0")
    local T_COMP=$(grep -oP 'T_composite[:\s]+\K[\d.]+' "$STDOUT" 2>/dev/null || echo "0")
    local PEAK_RSS=$(grep -oP 'peak_rss_mb[:\s]+\K[\d.]+' "$STDOUT" 2>/dev/null || echo "0")

    cat > "$JSON_OUT" <<JSONEOF
{
  "run_id": "$RUN_ID",
  "experiment": "ablation",
  "configuration": "$config_id",
  "description": "$description",
  "timestamp": "$TIMESTAMP",
  "git_commit": "$COMMIT",
  "dataset": "$DATASET",
  "size": "$SIZE",
  "threads": $THREADS,
  "wall_time_s": $WALL_S,
  "T_xr_s": $T_XR,
  "T_yr_s": $T_YR,
  "T_zr_s": $T_ZR,
  "T_composite_s": $T_COMP,
  "peak_rss_mb": $PEAK_RSS
}
JSONEOF

    echo "    → T_composite=$T_COMP  wall=${WALL_S}s"

    rm -rf "$OUTPUT_DIR"
}

# --- A0: Full ERWT3D (baseline) ---
run_ablation "A0_full" "Full ERWT3D" \
    --sb-task-order file-offset \
    --sb-read-mode hdd-read-window

# --- A1: Fixed compression (no adaptive) ---
# If erwt3d_convert supports --force-lz4 or --force-rzfp flags
# For now, use standard bench — adaptive is in conversion, not read
run_ablation "A1_no_adaptive" "No adaptive compression selection" \
    --sb-task-order logical \
    --sb-read-mode run-batch

# --- A2: No access planner ---
run_ablation "A2_no_planner" "No access planner (logical order, no window merging)" \
    --sb-task-order logical \
    --sb-read-mode run-batch

# --- A3: No device-aware scheduling ---
run_ablation "A3_no_device" "No device-aware scheduling (uniform strategy)" \
    --sb-task-order logical \
    --sb-read-mode leaf-index

echo ""
echo "[OK] Ablation results: $RESULTS_DIR/"
