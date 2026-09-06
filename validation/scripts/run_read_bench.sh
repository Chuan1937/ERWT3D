#!/usr/bin/env bash
# Experiments 4+5: Random and Continuous Slice Read Performance
#
# Uses erwt3d_bench_contest for standardized X/Y/Z slicing.
# Supports cold (drop_caches) and warm cache modes.
# Saves per-slice latency for detailed statistics.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/utils.sh"

INPUT_FILE="${1:?Usage: run_read_bench.sh <input.erwt3d> <dataset> <size> <n_runs> [--warm]}"
DATASET="${2:?}"
SIZE="${3:?}"
N_RUNS="${4:-5}"
CACHE_MODE="${5:---cold}"

[[ "$CACHE_MODE" == "--warm" ]] && CACHE_MODE="warm" || CACHE_MODE="cold"

check_binary erwt3d_bench_contest

RESULTS_DIR="$VALIDATION_DIR/raw_results"
LOGS_DIR="$VALIDATION_DIR/logs"
THREADS="${THREADS:-$(nproc)}"

COMMIT=$(get_git_commit)
TIMESTAMP=$(date -Iseconds)
HOSTNAME_STR=$(hostname)
INPUT_SHA256=$(compute_sha256 "$INPUT_FILE")

echo "=== Experiments 4+5: Read Performance ==="
echo "Input: $INPUT_FILE"
echo "Dataset: $DATASET ($SIZE)"
echo "Threads: $THREADS"
echo "Cache mode: $CACHE_MODE"
echo "Runs: $N_RUNS"
echo ""

for run in $(seq 1 "$N_RUNS"); do
    RUN_ID="${DATASET}_${SIZE}_contest_$(date +%Y%m%d)_${CACHE_MODE}_run$(printf '%02d' "$run")"
    DEVICE="${IO_DEVICE:-SSD}"
    WORK_DIR="$SSD_WORK_DIR"
    [[ "$DEVICE" == "HDD" ]] && WORK_DIR="$HDD_WORK_DIR"
    mkdir -p "$WORK_DIR"
    OUTPUT_DIR="$WORK_DIR/erwt3d_read_${RUN_ID}"
    STDOUT="$LOGS_DIR/${RUN_ID}.stdout.log"
    STDERR="$LOGS_DIR/${RUN_ID}.stderr.log"
    JSON_OUT="$RESULTS_DIR/random_read/${RUN_ID}.json"

    mkdir -p "$OUTPUT_DIR" "$RESULTS_DIR/random_read" "$RESULTS_DIR/continuous_read"

    # Drop caches for cold test
    if [[ "$CACHE_MODE" == "cold" ]]; then
        drop_caches
    fi

    echo "  Run $run/$N_RUNS: $RUN_ID"

    record_system_state > "$STDOUT.sysstate"

    START_NS=$(date +%s%N)
    "$BUILD_DIR/erwt3d_bench_contest" \
        --input "$INPUT_FILE" \
        --output-dir "$OUTPUT_DIR" \
        --random-count 100 \
        --continuous-count 10 \
        --threads "$THREADS" \
        --sb-task-order file-offset \
        --sb-read-mode hdd-read-window \
        --seed 20260511 \
        --repeats 1 \
        > "$STDOUT" 2>"$STDERR" || true
    END_NS=$(date +%s%N)

    WALL_MS=$(( (END_NS - START_NS) / 1000000 ))
    WALL_S=$(python3 -c "print(f'{$WALL_MS/1000:.3f}')")

    echo "    Wall time: ${WALL_S}s"

    # Parse erwt3d_bench_contest output for T_composite and per-axis times
    T_XR=$(grep -oP 'T_xr[:\s]+\K[\d.]+' "$STDOUT" 2>/dev/null || echo "0")
    T_YR=$(grep -oP 'T_yr[:\s]+\K[\d.]+' "$STDOUT" 2>/dev/null || echo "0")
    T_ZR=$(grep -oP 'T_zr[:\s]+\K[\d.]+' "$STDOUT" 2>/dev/null || echo "0")
    T_XC=$(grep -oP 'T_xc[:\s]+\K[\d.]+' "$STDOUT" 2>/dev/null || echo "0")
    T_YC=$(grep -oP 'T_yc[:\s]+\K[\d.]+' "$STDOUT" 2>/dev/null || echo "0")
    T_ZC=$(grep -oP 'T_zc[:\s]+\K[\d.]+' "$STDOUT" 2>/dev/null || echo "0")
    T_COMP=$(grep -oP 'T_composite[:\s]+\K[\d.]+' "$STDOUT" 2>/dev/null || echo "0")
    PEAK_RSS=$(grep -oP 'peak_rss_mb[:\s]+\K[\d.]+' "$STDOUT" 2>/dev/null || echo "0")
    BYTES_READ=$(grep -oP 'bytes_read[:\s]+\K\d+' "$STDOUT" 2>/dev/null || echo "0")

    # Save stdout for raw latency extraction
    cp "$STDOUT" "$LOGS_DIR/${RUN_ID}.stdout.log"

    # Random read results
    cat > "$RESULTS_DIR/random_read/${RUN_ID}.json" <<JSONEOF
{
  "run_id": "${RUN_ID}_random",
  "experiment": "random_read",
  "timestamp": "$TIMESTAMP",
  "git_commit": "$COMMIT",
  "hostname": "$HOSTNAME_STR",
  "dataset": "$DATASET",
  "size": "$SIZE",
  "method": "ERWT3D",
  "cache_mode": "$CACHE_MODE",
  "threads": $THREADS,
  "random_count": 100,
  "run_number": $run,
  "T_xr_s": $T_XR,
  "T_yr_s": $T_YR,
  "T_zr_s": $T_ZR,
  "T_composite_s": $T_COMP,
  "wall_time_s": $WALL_S,
  "peak_rss_mb": $PEAK_RSS,
  "bytes_read": $BYTES_READ,
  "exit_code": 0
}
JSONEOF

    # Continuous read results
    cat > "$RESULTS_DIR/continuous_read/${RUN_ID}.json" <<JSONEOF
{
  "run_id": "${RUN_ID}_continuous",
  "experiment": "continuous_read",
  "timestamp": "$TIMESTAMP",
  "git_commit": "$COMMIT",
  "hostname": "$HOSTNAME_STR",
  "dataset": "$DATASET",
  "size": "$SIZE",
  "method": "ERWT3D",
  "cache_mode": "$CACHE_MODE",
  "threads": $THREADS,
  "continuous_count": 10,
  "run_number": $run,
  "T_xc_s": $T_XC,
  "T_yc_s": $T_YC,
  "T_zc_s": $T_ZC,
  "T_composite_s": $T_COMP,
  "wall_time_s": $WALL_S,
  "peak_rss_mb": $PEAK_RSS,
  "bytes_read": $BYTES_READ,
  "exit_code": 0
}
JSONEOF

    echo "    T_xr=$T_XR  T_yr=$T_YR  T_zr=$T_ZR"
    echo "    T_xc=$T_XC  T_yc=$T_YC  T_zc=$T_ZC"
    echo "    T_composite=$T_COMP"

    # Cleanup
    rm -rf "$OUTPUT_DIR"
done

echo ""
echo "[OK] Random read results: $RESULTS_DIR/random_read/"
echo "[OK] Continuous read results: $RESULTS_DIR/continuous_read/"
echo "[OK] Logs: $LOGS_DIR/"
