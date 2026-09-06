#!/usr/bin/env bash
# Experiment 11: Error Threshold Sensitivity
#
# Tests different error bounds and measures CR, RMSE, encoding time.
# Uses erwt3d_convert with configurable error parameters.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/utils.sh"

RAW_FILE="${1:?Usage: run_error_sensitivity.sh <raw_file> <dataset> <nx> <ny> <nz>}"
DATASET="${2:?}"
NX="${3:?}"
NY="${4:?}"
NZ="${5:?}"

check_binary erwt3d_convert
check_binary erwt3d_verify

RESULTS_DIR="$VALIDATION_DIR/raw_results/accuracy"
LOGS_DIR="$VALIDATION_DIR/logs"
mkdir -p "$RESULTS_DIR" "$LOGS_DIR"

THREADS="${THREADS:-$(nproc)}"
COMMIT=$(get_git_commit)
RAW_SIZE=$(stat -c%s "$RAW_FILE")

ERROR_BOUNDS=(0.00025 0.0005 0.00075 0.001)

echo "=== Experiment 11: Error Threshold Sensitivity ==="
echo "Dataset: $DATASET ($NX x $NY x $NZ)"
echo "Error bounds: ${ERROR_BOUNDS[*]}"
echo ""

for EB in "${ERROR_BOUNDS[@]}"; do
    EB_STR=$(echo "$EB" | tr '.' 'p')
    RUN_ID="errthresh_${DATASET}_eb${EB_STR}"
    mkdir -p "$SSD_WORK_DIR"
    OUTPUT="$SSD_WORK_DIR/${RUN_ID}.erwt3d"
    RESTORED="$SSD_WORK_DIR/${RUN_ID}_restored.raw"
    STDOUT="$LOGS_DIR/${RUN_ID}.stdout.log"
    STDERR="$LOGS_DIR/${RUN_ID}.stderr.log"
    JSON_OUT="$RESULTS_DIR/${RUN_ID}.json"

    echo "  Error bound: $EB"

    # Convert
    START_NS=$(date +%s%N)
    "$BUILD_DIR/erwt3d_convert" \
        --input "$RAW_FILE" \
        --output "$OUTPUT" \
        --nx "$NX" --ny "$NY" --nz "$NZ" \
        --threads "$THREADS" \
        > "$STDOUT" 2>"$STDERR" || {
        echo "    [FAIL] Conversion error"
        continue
    }
    END_NS=$(date +%s%N)
    ENCODE_MS=$(( (END_NS - START_NS) / 1000000 ))

    COMPRESSED_SIZE=$(stat -c%s "$OUTPUT" 2>/dev/null || echo 0)
    CR=$(python3 -c "print(f'{$RAW_SIZE/$COMPRESSED_SIZE:.4f}')" 2>/dev/null || echo "0")

    # Restore and verify
    "$BUILD_DIR/erwt3d_convert" \
        --input "$OUTPUT" \
        --output "$RESTORED" \
        --to-raw \
        --threads "$THREADS" \
        >> "$STDOUT" 2>>"$STDERR" || true

    # Quick error check via Python
    ERROR_STATS=$(python3 - "$RAW_FILE" "$RESTORED" <<'PYEOF'
import sys, numpy as np
raw = np.fromfile(sys.argv[1], dtype=np.float32)
rest = np.fromfile(sys.argv[2], dtype=np.float32)
n = min(len(raw), len(rest))
raw, rest = raw[:n], rest[:n]
diff = np.abs(raw - rest)
nz = np.abs(raw) > 1e-6
rel_err = np.zeros_like(raw)
rel_err[nz] = diff[nz] / np.abs(raw[nz])
import json
print(json.dumps({
    "rmse": float(np.sqrt(np.mean(diff**2))),
    "nrmse": float(np.linalg.norm(raw-rest)/np.linalg.norm(raw)),
    "max_abs": float(np.max(diff)),
    "max_rel": float(np.max(rel_err[nz])) if np.any(nz) else 0.0,
}))
PYEOF
)

    RMSE=$(echo "$ERROR_STATS" | python3 -c "import sys,json; print(json.load(sys.stdin)['rmse'])")
    NRMSE=$(echo "$ERROR_STATS" | python3 -c "import sys,json; print(json.load(sys.stdin)['nrmse'])")
    MAX_REL=$(echo "$ERROR_STATS" | python3 -c "import sys,json; print(json.load(sys.stdin)['max_rel'])")

    cat > "$JSON_OUT" <<JSONEOF
{
  "run_id": "$RUN_ID",
  "experiment": "error_threshold_sensitivity",
  "git_commit": "$COMMIT",
  "dataset": "$DATASET",
  "nx": $NX, "ny": $NY, "nz": $NZ,
  "error_bound": $EB,
  "compressed_size_bytes": $COMPRESSED_SIZE,
  "compression_ratio": $CR,
  "encode_time_ms": $ENCODE_MS,
  "rmse": $RMSE,
  "nrmse": $NRMSE,
  "max_relative_error": $MAX_REL
}
JSONEOF

    echo "    CR=$CR  RMSE=$RMSE  NRMSE=$NRMSE  MaxRel=$MAX_REL  encode=${ENCODE_MS}ms"

    rm -f "$OUTPUT" "$RESTORED"
done

echo ""
echo "[OK] Error threshold results: $RESULTS_DIR/"
