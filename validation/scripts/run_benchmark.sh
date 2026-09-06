#!/usr/bin/env bash
# Core benchmark runner for ERWT3D paper experiments
# Usage: run_benchmark.sh <method> <dataset> <size> <axis> <pattern> <run_num> [extra_args...]
#
# Example:
#   run_benchmark.sh erwt3d datasetA 20GB x random 1 --threads 8 --io-profile auto
#   run_benchmark.sh zfp datasetA 20GB x random 1 --mode fixed-accuracy --tolerance 0.001
#   run_benchmark.sh raw datasetB 50GB z continuous 1
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/utils.sh"

METHOD="${1:?Usage: run_benchmark.sh <method> <dataset> <size> <axis> <pattern> <run_num>}"
DATASET="${2:?}"
SIZE="${3:?}"
AXIS="${4:?}"
PATTERN="${5:?}"
RUN_NUM="${6:?}"
shift 6
EXTRA_ARGS=("$@")

# --- Validate inputs ---
check_binary erwt3d_contest

DEVICE="${IO_DEVICE:-SSD}"
COMMIT=$(get_git_commit)
RUN_ID=$(generate_run_id "$DEVICE" "$METHOD" "$DATASET" "$SIZE" "$AXIS" "$PATTERN" "$RUN_NUM")

echo "=== Benchmark Run ==="
echo "Run ID: $RUN_ID"
echo "Method: $METHOD"
echo "Dataset: $DATASET ($SIZE)"
echo "Axis: $AXIS / Pattern: $PATTERN"
echo "Run: $RUN_NUM"
echo "Commit: $COMMIT"
echo "Extra args: ${EXTRA_ARGS[*]:-none}"
echo ""

# --- Prepare paths ---
RESULT_CAT="random_read"
[[ "$PATTERN" == "continuous" ]] && RESULT_CAT="continuous_read"

JSON_PATH=$(result_json_path "$RUN_ID" "$RESULT_CAT")
CSV_PATH=$(result_csv_path "$RUN_ID" "$RESULT_CAT")
STDOUT_LOG=$(log_stdout_path "$RUN_ID")
STDERR_LOG=$(log_stderr_path "$RUN_ID")

mkdir -p "$(dirname "$JSON_PATH")" "$(dirname "$STDOUT_LOG")"

# --- Record system state ---
SYSTEM_STATE=$(record_system_state)

# --- Drop caches for cold test ---
if [[ "${CACHE_MODE:-cold}" == "cold" ]]; then
    drop_caches
fi

# --- Build command ---
INPUT_FILE="${DATASET_DIR:?Set DATASET_DIR}/dataset_${SIZE}.erwt3d"
OUTPUT_DIR="/tmp/erwt3d_bench_${RUN_ID}"
POSITIONS_FILE="$VALIDATION_DIR/workloads/${SIZE}/${PATTERN}_${AXIS}.txt"

check_file "$INPUT_FILE"
check_file "$POSITIONS_FILE"

TIMESTAMP_START=$(date -Iseconds)
WALL_START=$(date +%s%N)

case "$METHOD" in
    erwt3d)
        "$BUILD_DIR/erwt3d_contest" \
            --input "$INPUT_FILE" \
            --output-dir "$OUTPUT_DIR" \
            --positions-file "$POSITIONS_FILE" \
            --axis "$AXIS" \
            --threads "${THREADS:-$(nproc)}" \
            --io-profile "${IO_PROFILE:-auto}" \
            "${EXTRA_ARGS[@]}" \
            > "$STDOUT_LOG" 2>"$STDERR_LOG"
        ;;
    raw|lz4|zfp|hdf5)
        echo "[INFO] Baseline method '$METHOD' — call appropriate binary/script"
        # Placeholder: actual baseline runners to be implemented per method
        "$BUILD_DIR/erwt3d_contest" \
            --input "$INPUT_FILE" \
            --output-dir "$OUTPUT_DIR" \
            --positions-file "$POSITIONS_FILE" \
            --axis "$AXIS" \
            --threads "${THREADS:-$(nproc)}" \
            "${EXTRA_ARGS[@]}" \
            > "$STDOUT_LOG" 2>"$STDERR_LOG"
        ;;
    *)
        echo "[ERROR] Unknown method: $METHOD"
        exit 1
        ;;
esac

WALL_END=$(date +%s%N)
TIMESTAMP_END=$(date -Iseconds)
WALL_TIME_MS=$(( (WALL_END - WALL_START) / 1000000 ))
WALL_TIME_S=$(python3 -c "print(f'{$WALL_TIME_MS/1000:.3f}')")

# --- Extract metrics from output ---
# Parse erwt3d_contest output for timing, throughput, RSS, etc.
BYTES_READ=$(grep -oP 'bytes_read[:\s]+\K\d+' "$STDOUT_LOG" 2>/dev/null || echo 0)
BYTES_WRITTEN=$(grep -oP 'bytes_written[:\s]+\K\d+' "$STDOUT_LOG" 2>/dev/null || echo 0)
PEAK_RSS_MB=$(grep -oP 'peak_rss_mb[:\s]+\K[\d.]+' "$STDOUT_LOG" 2>/dev/null || echo 0)
CPU_TIME_S=$(grep -oP 'cpu_time_s[:\s]+\K[\d.]+' "$STDOUT_LOG" 2>/dev/null || echo 0)

# --- Write result JSON ---
cat > "$JSON_PATH" <<JSONEOF
{
  "run_id": "$RUN_ID",
  "timestamp_start": "$TIMESTAMP_START",
  "timestamp_end": "$TIMESTAMP_END",
  "git_commit": "$COMMIT",
  "hostname": "$(hostname)",
  "device": "$DEVICE",
  "dataset": "$DATASET",
  "size": "$SIZE",
  "method": "$METHOD",
  "method_parameters": {},
  "axis": "$AXIS",
  "access_pattern": "$PATTERN",
  "run_number": $RUN_NUM,
  "cache_mode": "${CACHE_MODE:-cold}",
  "threads": ${THREADS:-$(nproc)},
  "wall_time_s": $WALL_TIME_S,
  "wall_time_ms": $WALL_TIME_MS,
  "peak_rss_mb": $PEAK_RSS_MB,
  "bytes_read": $BYTES_READ,
  "bytes_written": $BYTES_WRITTEN,
  "exit_code": 0,
  "system_state": $(echo "$SYSTEM_STATE" | python3 -c 'import sys,json; print(json.dumps(sys.stdin.read()))' 2>/dev/null || echo '""')
}
JSONEOF

echo ""
echo "[OK] Result: $JSON_PATH"
echo "[OK] Stdout: $STDOUT_LOG"
echo "[OK] Stderr: $STDERR_LOG"
echo "[OK] Wall time: ${WALL_TIME_S}s"

# Cleanup
rm -rf "$OUTPUT_DIR" 2>/dev/null || true
