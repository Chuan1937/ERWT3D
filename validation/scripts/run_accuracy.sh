#!/usr/bin/env bash
# Experiment 3: Reconstruction Accuracy
#
# Tests: max_abs_error, RMSE, NRMSE, max/mean/median/p95/p99/p99.9 relative error
#        zero preservation, special values
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/utils.sh"

DATASET="${1:?Usage: run_accuracy.sh <dataset> <raw_file> <nx> <ny> <nz>}"
RAW_FILE="${2:?}"
NX="${3:?}"
NY="${4:?}"
NZ="${5:?}"

check_binary erwt3d_convert
check_binary erwt3d_verify

RESULTS_DIR="$VALIDATION_DIR/raw_results/accuracy"
LOGS_DIR="$VALIDATION_DIR/logs"
mkdir -p "$RESULTS_DIR" "$LOGS_DIR"

COMMIT=$(get_git_commit)
TIMESTAMP=$(date -Iseconds)
RAW_SHA256=$(compute_sha256 "$RAW_FILE")
THREADS="${THREADS:-$(nproc)}"

echo "=== Experiment 3: Reconstruction Accuracy ==="
echo "Dataset: $DATASET ($NX x $NY x $NZ)"
echo ""

# --- Step 1: Convert raw → erwt3d ---
mkdir -p "$SSD_WORK_DIR"
CONVERTED="$SSD_WORK_DIR/accuracy_${DATASET}.erwt3d"
CONVERT_LOG="$LOGS_DIR/accuracy_${DATASET}_convert.log"

echo "  Converting raw → erwt3d..."
"$BUILD_DIR/erwt3d_convert" \
    --input "$RAW_FILE" \
    --output "$CONVERTED" \
    --nx "$NX" --ny "$NY" --nz "$NZ" \
    --threads "$THREADS" \
    > "$CONVERT_LOG" 2>&1 || {
    echo "[ERROR] Conversion failed. See $CONVERT_LOG"
    exit 1
}

CONVERTED_SIZE=$(stat -c%s "$CONVERTED")
RAW_SIZE=$(stat -c%s "$RAW_FILE")
CR=$(python3 -c "print(f'{$RAW_SIZE/$CONVERTED_SIZE:.4f}')")

echo "  Converted: $CONVERTED ($CONVERTED_SIZE bytes, CR=$CR)"

# --- Step 2: Convert back → raw ---
RESTORED="$SSD_WORK_DIR/accuracy_${DATASET}_restored.raw"
RESTORE_LOG="$LOGS_DIR/accuracy_${DATASET}_restore.log"

echo "  Restoring erwt3d → raw..."
"$BUILD_DIR/erwt3d_convert" \
    --input "$CONVERTED" \
    --output "$RESTORED" \
    --nx "$NX" --ny "$NY" --nz "$NZ" \
    --to-raw \
    --threads "$THREADS" \
    > "$RESTORE_LOG" 2>&1 || {
    echo "[ERROR] Restore failed. See $RESTORE_LOG"
    exit 1
}

# --- Step 3: Run erwt3d_verify ---
VERIFY_LOG="$LOGS_DIR/accuracy_${DATASET}_verify.log"
echo "  Verifying..."

"$BUILD_DIR/erwt3d_verify" \
    --raw "$RAW_FILE" \
    --erwt3d "$CONVERTED" \
    --nx "$NX" --ny "$NY" --nz "$NZ" \
    --rel-tol 0.001 \
    > "$VERIFY_LOG" 2>&1 || true

echo "  Verify output:"
cat "$VERIFY_LOG"
echo ""

# --- Step 4: Compute detailed error statistics via Python ---
echo "  Computing detailed error statistics..."
python3 - "$RAW_FILE" "$RESTORED" "$DATASET" "$COMMIT" "$TIMESTAMP" "$RESULTS_DIR" "$NX" "$NY" "$NZ" "$CONVERTED_SIZE" "$RAW_SIZE" "$CR" <<'PYEOF'
import sys, json, numpy as np
from pathlib import Path

raw_path, restored_path, dataset, commit, timestamp, results_dir = sys.argv[1:7]
nx, ny, nz = int(sys.argv[7]), int(sys.argv[8]), int(sys.argv[9])
comp_size, raw_size, cr = int(sys.argv[10]), int(sys.argv[11]), float(sys.argv[12])

raw = np.fromfile(raw_path, dtype=np.float32)
restored = np.fromfile(restored_path, dtype=np.float32)

n = min(len(raw), len(restored))
raw = raw[:n]
restored = restored[:n]

diff = raw - restored
abs_diff = np.abs(diff)

# Handle near-zero reference values
zero_mask = np.abs(raw) < 1e-6
nonzero_mask = ~zero_mask

# Absolute error
max_abs_error = float(np.max(abs_diff))

# RMSE
rmse = float(np.sqrt(np.mean(diff**2)))

# NRMSE (L2 norm based)
norm_raw = float(np.linalg.norm(raw))
nrmse = float(np.linalg.norm(diff) / norm_raw) if norm_raw > 0 else 0.0

# Relative error (for nonzero values)
rel_error = np.zeros_like(raw)
rel_error[nonzero_mask] = abs_diff[nonzero_mask] / np.abs(raw[nonzero_mask])

max_rel_error = float(np.max(rel_error[nonzero_mask])) if np.any(nonzero_mask) else 0.0
mean_rel_error = float(np.mean(rel_error[nonzero_mask])) if np.any(nonzero_mask) else 0.0
median_rel_error = float(np.median(rel_error[nonzero_mask])) if np.any(nonzero_mask) else 0.0
p95_rel = float(np.percentile(rel_error[nonzero_mask], 95)) if np.any(nonzero_mask) else 0.0
p99_rel = float(np.percentile(rel_error[nonzero_mask], 99)) if np.any(nonzero_mask) else 0.0
p999_rel = float(np.percentile(rel_error[nonzero_mask], 99.9)) if np.any(nonzero_mask) else 0.0

# Zero preservation
zero_exact = int(np.sum((raw == 0) & (restored == 0)))
zero_original = int(np.sum(raw == 0))
zero_preservation = zero_exact / zero_original if zero_original > 0 else 1.0

# NaN/Inf counts
nan_orig = int(np.sum(np.isnan(raw)))
nan_rest = int(np.sum(np.isnan(restored)))
inf_orig = int(np.sum(np.isinf(raw)))
inf_rest = int(np.sum(np.isinf(restored)))

# Subnormal count
subnormal_orig = int(np.sum((np.abs(raw) < np.finfo(np.float32).tiny) & (raw != 0) & ~np.isnan(raw)))

result = {
    "run_id": f"accuracy_{dataset}_ERWT3D",
    "experiment": "accuracy",
    "timestamp": timestamp,
    "git_commit": commit,
    "dataset": dataset,
    "nx": nx, "ny": ny, "nz": nz,
    "method": "ERWT3D",
    "raw_size_bytes": raw_size,
    "compressed_size_bytes": comp_size,
    "compression_ratio": cr,
    "max_abs_error": max_abs_error,
    "rmse": rmse,
    "nrmse": nrmse,
    "max_relative_error": max_rel_error,
    "mean_relative_error": mean_rel_error,
    "median_relative_error": median_rel_error,
    "p95_relative_error": p95_rel,
    "p99_relative_error": p99_rel,
    "p999_relative_error": p999_rel,
    "zero_preservation_rate": zero_preservation,
    "zero_original_count": zero_original,
    "zero_decoded_exact": zero_exact,
    "nan_original": nan_orig,
    "nan_decoded": nan_rest,
    "inf_original": inf_orig,
    "inf_decoded": inf_rest,
    "subnormal_original": subnormal_orig,
    "total_elements": n,
    "elements_within_tolerance": int(np.sum(rel_error[nonzero_mask] < 0.001)) if np.any(nonzero_mask) else n,
    "tolerance_compliance_rate": float(np.sum(rel_error[nonzero_mask] < 0.001) / np.sum(nonzero_mask)) if np.any(nonzero_mask) else 1.0,
}

out_path = Path(results_dir) / f"accuracy_{dataset}_ERWT3D.json"
with open(out_path, 'w') as f:
    json.dump(result, f, indent=2)

print(f"  Max abs error:      {max_abs_error:.6e}")
print(f"  RMSE:               {rmse:.6e}")
print(f"  NRMSE:              {nrmse:.6e}")
print(f"  Max relative error: {max_rel_error:.6e}")
print(f"  P99 relative error: {p99_rel:.6e}")
print(f"  Zero preservation:  {zero_preservation:.4f}")
print(f"  Tolerance compliance: {result['tolerance_compliance_rate']:.4f}")
print(f"\n  Result: {out_path}")
PYEOF

# Cleanup
rm -f "$CONVERTED" "$RESTORED"

echo ""
echo "[OK] Accuracy results: $RESULTS_DIR"
