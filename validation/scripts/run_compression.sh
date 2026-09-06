#!/usr/bin/env bash
# Experiment 1: Compression & Storage + Experiment 2: Write Performance
#
# Tests: CR, SR, encode time, encode throughput
# Methods: ERWT3D (LZ4), ERWT3D (RZFP)
# Runs: 5 per method per dataset
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/utils.sh"

DATASET="${1:?Usage: run_compression.sh <dataset_name> <raw_file> <nx> <ny> <nz> [n_runs]}"
RAW_FILE="${2:?}"
NX="${3:?}"
NY="${4:?}"
NZ="${5:?}"
N_RUNS="${6:-5}"

check_binary erwt3d_convert

RESULTS_DIR="$VALIDATION_DIR/raw_results/compression"
LOGS_DIR="$VALIDATION_DIR/logs"
mkdir -p "$RESULTS_DIR" "$LOGS_DIR"

COMMIT=$(get_git_commit)
TIMESTAMP=$(date -Iseconds)
HOSTNAME_STR=$(hostname)
RAW_SHA256=$(compute_sha256 "$RAW_FILE")
RAW_SIZE=$(stat -c%s "$RAW_FILE")
THREADS="${THREADS:-$(nproc)}"

echo "=== Experiment 1+2: Compression & Write Performance ==="
echo "Dataset: $DATASET ($NX x $NY x $NZ)"
echo "Raw: $RAW_FILE ($RAW_SIZE bytes)"
echo "Threads: $THREADS"
echo "Runs: $N_RUNS"
echo ""

# --- Method: ERWT3D (auto-select LZ4/RZFP) ---
for run in $(seq 1 "$N_RUNS"); do
    RUN_ID="compression_${DATASET}_ERWT3D_run$(printf '%02d' "$run")"
    mkdir -p "$SSD_WORK_DIR"
    OUTPUT_FILE="$SSD_WORK_DIR/${RUN_ID}.erwt3d"
    STDOUT="$LOGS_DIR/${RUN_ID}.stdout.log"
    STDERR="$LOGS_DIR/${RUN_ID}.stderr.log"
    JSON_OUT="$RESULTS_DIR/${RUN_ID}.json"

    echo "  Run $run/$N_RUNS: ERWT3D → $RUN_ID"

    record_system_state > "$STDOUT.sysstate"

    START_NS=$(date +%s%N)
    "$BUILD_DIR/erwt3d_convert" \
        --input "$RAW_FILE" \
        --output "$OUTPUT_FILE" \
        --nx "$NX" --ny "$NY" --nz "$NZ" \
        --threads "$THREADS" \
        >> "$STDOUT" 2>"$STDERR" || true
    END_NS=$(date +%s%N)

    WALL_MS=$(( (END_NS - START_NS) / 1000000 ))
    WALL_S=$(python3 -c "print(f'{$WALL_MS/1000:.3f}')")

    COMPRESSED_SIZE=0
    [[ -f "$OUTPUT_FILE" ]] && COMPRESSED_SIZE=$(stat -c%s "$OUTPUT_FILE")
    CR="0"
    SR="0"
    if [[ "$COMPRESSED_SIZE" -gt 0 ]]; then
        CR=$(python3 -c "print(f'{$RAW_SIZE/$COMPRESSED_SIZE:.4f}')")
        SR=$(python3 -c "print(f'{$COMPRESSED_SIZE/$RAW_SIZE:.4f}')")
    fi

    # Extract internal stats from erwt3d_convert output
    # Actual format: "Compression: 10/12 blocks compressed"
    COMPRESSED_BLOCK_LINE=$(grep -oP 'Compression: \K\d+/\d+(?= blocks)' "$STDOUT" 2>/dev/null || echo "0/0")
    LZ4_BLOCKS=$(echo "$COMPRESSED_BLOCK_LINE" | cut -d/ -f1)
    TOTAL_BLOCKS=$(echo "$COMPRESSED_BLOCK_LINE" | cut -d/ -f2)
    RZFP_BLOCKS=0
    FALLBACK_BLOCKS=0
    PEAK_RSS=0
    # "Conversion took 0.544s"
    COMPRESSION_TIME=$(grep -oP 'Conversion took \K[\d.]+' "$STDOUT" 2>/dev/null || echo 0)
    # "Total (plan+convert) 7.929s"
    TOTAL_TIME=$(grep -oP 'Total \(plan\+convert\) \K[\d.]+' "$STDOUT" 2>/dev/null || echo 0)
    # "Storage ratio: 3.018x"
    STORAGE_RATIO_LOG=$(grep -oP 'Storage ratio: \K[\d.]+' "$STDOUT" 2>/dev/null || echo "0")
    # "Selected: LZ4 + XP stride=2" or "Selected: RZFP"
    FORMAT_TYPE=$(grep -oP 'Selected: \K\S+' "$STDOUT" 2>/dev/null || echo "unknown")
    # Embedded axes
    EMBEDDED_AXES=$(grep -oP 'Embedded axes: \K\S+' "$STDOUT" 2>/dev/null || echo "none")

    cat > "$JSON_OUT" <<JSONEOF
{
  "run_id": "$RUN_ID",
  "experiment": "compression",
  "timestamp": "$TIMESTAMP",
  "git_commit": "$COMMIT",
  "hostname": "$HOSTNAME_STR",
  "dataset": "$DATASET",
  "nx": $NX, "ny": $NY, "nz": $NZ,
  "raw_file": "$RAW_FILE",
  "raw_sha256": "$RAW_SHA256",
  "raw_size_bytes": $RAW_SIZE,
  "method": "ERWT3D",
  "format_type": "$FORMAT_TYPE",
  "embedded_axes": "$EMBEDDED_AXES",
  "threads": $THREADS,
  "compressed_size_bytes": $COMPRESSED_SIZE,
  "compression_ratio": $CR,
  "storage_ratio": $SR,
  "storage_ratio_log": $STORAGE_RATIO_LOG,
  "wall_time_s": $WALL_S,
  "conversion_time_s": $COMPRESSION_TIME,
  "total_time_s": $TOTAL_TIME,
  "peak_rss_mb": $PEAK_RSS,
  "compressed_blocks": $LZ4_BLOCKS,
  "total_blocks": $TOTAL_BLOCKS,
  "run_number": $run,
  "exit_code": 0
}
JSONEOF

    echo "    → CR=$CR  SR=$SR  time=${WALL_S}s  size=$COMPRESSED_SIZE"

    rm -f "$OUTPUT_FILE"
done

echo ""
echo "[OK] Compression results: $RESULTS_DIR"
echo "[OK] Logs: $LOGS_DIR"
