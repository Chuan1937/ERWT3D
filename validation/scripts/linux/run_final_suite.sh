#!/usr/bin/env bash
# FINAL orchestrator: runs all experiments for the paper.
# Usage: run_final_suite.sh [--skip-convert] [--skip-50gb-check]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VALIDATION_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
PROJECT_ROOT="$(cd "$VALIDATION_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

export ERWT3D_THREADS="${ERWT3D_THREADS:-8}"
export ERWT3D_RESULT_ROOT="/mnt/f/CUP/results/raw_results_final"
export ERWT3D_DATASET_ROOT="/mnt/f/CUP/datasets"
export ERWT3D_SSD_ROOT="/mnt/f/CUP/erwt3d-paper"
export ERWT3D_HDD_ROOT="/mnt/d/erwt3d-paper"
export ERWT3D_BUILD_DIR="$BUILD_DIR"
export ERWT3D_ALGORITHM_COMMIT="$(git -C "$PROJECT_ROOT" rev-parse HEAD)"

SSD_ERWT3D="$ERWT3D_SSD_ROOT/erwt3d"
HDD_ERWT3D="$ERWT3D_HDD_ROOT/erwt3d"
mkdir -p "$SSD_ERWT3D" "$HDD_ERWT3D" "$ERWT3D_RESULT_ROOT"/{random_read,continuous_read,accuracy,logs,slice_outputs}

ALGO_SHORT="${ERWT3D_ALGORITHM_COMMIT:0:7}"
echo "========================================"
echo "ERWT3D FINAL Benchmark Suite"
echo "Algorithm commit: $ERWT3D_ALGORITHM_COMMIT"
echo "Threads: $ERWT3D_THREADS"
echo "Results: $ERWT3D_RESULT_ROOT"
echo "========================================"

# ============================================================
# STEP 4: 20GB Storage sanity check
# ============================================================
echo ""
echo "=== STEP 4: 20GB Auto Storage Check ==="
RAW_20GB="$ERWT3D_DATASET_ROOT/cup_3d_small.dat"
[[ -f "$RAW_20GB" ]] || { echo "[ERROR] Missing $RAW_20GB"; exit 1; }
RAW_20GB_SIZE=$(stat -c%s "$RAW_20GB")
echo "Raw 20GB: $RAW_20GB ($RAW_20GB_SIZE bytes)"

# Use existing Auto-converted file or convert fresh
AUTO_20GB="$SSD_ERWT3D/20GB_A0_auto_final.erwt3d"
if [[ ! -f "$AUTO_20GB" ]]; then
    echo "Converting 20GB Auto..."
    "$BUILD_DIR/erwt3d_convert" --input "$RAW_20GB" --output "$AUTO_20GB" \
        --nx 801 --ny 2405 --nz 2501 --threads "$ERWT3D_THREADS" 2>&1 | tee "$ERWT3D_RESULT_ROOT/logs/convert_20gb_auto.log"
fi
AUTO_20GB_SIZE=$(stat -c%s "$AUTO_20GB")
AUTO_20GB_RATIO=$(python3 -c "print(f'{$AUTO_20GB_SIZE / $RAW_20GB_SIZE:.3f}')")
echo "Auto 20GB: $AUTO_20GB ($AUTO_20GB_SIZE bytes, ratio=${AUTO_20GB_RATIO}x)"

# ============================================================
# STEP 5: 20GB Auto FINAL SSD + HDD access
# ============================================================
echo ""
echo "=== STEP 5: 20GB Auto FINAL Access ==="
BENCH_SH="$SCRIPT_DIR/run_final_bench.sh"
chmod +x "$BENCH_SH"

for device in SSD HDD; do
    input="$AUTO_20GB"
    if [[ "$device" == HDD ]]; then
        input_hdd="$HDD_ERWT3D/20GB_A0_auto_final.erwt3d"
        if [[ ! -f "$input_hdd" ]]; then
            echo "Copying 20GB Auto to HDD..."
            cp --reflink=auto "$AUTO_20GB" "$input_hdd"
        fi
        input="$input_hdd"
    fi
    for axis in x y z; do
        for pattern in random continuous; do
            for run in 1 2 3 4 5; do
                echo "  20GB A0_auto_final $device $axis $pattern run$run"
                "$BENCH_SH" \
                    --input "$input" \
                    --dataset 20GB \
                    --format LZ4 \
                    --layout "LZ4+XYZ" \
                    --configuration "A0_auto_final" \
                    --device "$device" \
                    --axis "$axis" \
                    --pattern "$pattern" \
                    --run "$run" || echo "[WARN] Failed: 20GB A0 $device $axis $pattern run$run"
            done
        done
    done
done

# ============================================================
# STEP 6: 20GB Forced LZ4 FINAL
# ============================================================
echo ""
echo "=== STEP 6: 20GB Forced LZ4 ==="
LZ4_20GB="$SSD_ERWT3D/20GB_A1_force_lz4_final.erwt3d"
if [[ ! -f "$LZ4_20GB" ]]; then
    echo "Converting 20GB forced LZ4..."
    "$BUILD_DIR/erwt3d_convert" --input "$RAW_20GB" --output "$LZ4_20GB" \
        --nx 801 --ny 2405 --nz 2501 --threads "$ERWT3D_THREADS" --force-format lz4 2>&1 | tee "$ERWT3D_RESULT_ROOT/logs/convert_20gb_force_lz4.log"
fi

for axis in x y z; do
    for pattern in random continuous; do
        for run in 1 2 3 4 5; do
            echo "  20GB A1_force_lz4_final SSD $axis $pattern run$run"
            "$BENCH_SH" \
                --input "$LZ4_20GB" \
                --dataset 20GB \
                --format LZ4 \
                --layout "LZ4+XYZ" \
                --configuration "A1_force_lz4_final" \
                --device SSD \
                --axis "$axis" \
                --pattern "$pattern" \
                --run "$run" || echo "[WARN] Failed"
        done
    done
done

# ============================================================
# STEP 7: 20GB Forced RZFP FINAL
# ============================================================
echo ""
echo "=== STEP 7: 20GB Forced RZFP ==="
RZFP_20GB="$SSD_ERWT3D/20GB_A1_force_rzfp_final.erwt3d"
if [[ ! -f "$RZFP_20GB" ]]; then
    echo "Converting 20GB forced RZFP..."
    "$BUILD_DIR/erwt3d_convert" --input "$RAW_20GB" --output "$RZFP_20GB" \
        --nx 801 --ny 2405 --nz 2501 --threads "$ERWT3D_THREADS" --force-format rzfp 2>&1 | tee "$ERWT3D_RESULT_ROOT/logs/convert_20gb_force_rzfp.log"
fi

for axis in x y z; do
    for pattern in random continuous; do
        for run in 1 2 3 4 5; do
            echo "  20GB A1_force_rzfp_final SSD $axis $pattern run$run"
            "$BENCH_SH" \
                --input "$RZFP_20GB" \
                --dataset 20GB \
                --format RZFP \
                --layout "RZFP+XYZ" \
                --configuration "A1_force_rzfp_final" \
                --device SSD \
                --axis "$axis" \
                --pattern "$pattern" \
                --run "$run" || echo "[WARN] Failed"
        done
    done
done

# ============================================================
# STEP 8: 20GB Raw baseline SSD
# ============================================================
echo ""
echo "=== STEP 8: 20GB Raw Baseline ==="
for axis in x y z; do
    for pattern in random continuous; do
        for run in 1 2 3 4 5; do
            echo "  20GB raw_baseline SSD $axis $pattern run$run"
            "$BENCH_SH" \
                --input "$RAW_20GB" \
                --dataset 20GB \
                --format RAW \
                --layout "raw" \
                --configuration "raw_baseline" \
                --device SSD \
                --axis "$axis" \
                --pattern "$pattern" \
                --run "$run" || echo "[WARN] Failed"
        done
    done
done

# ============================================================
# STEP 9: F3 FINAL per-slice SSD
# ============================================================
echo ""
echo "=== STEP 9: F3 FINAL per-slice ==="
for f3ds in f3_amplitude f3_similarity; do
    RAW_F3="$ERWT3D_DATASET_ROOT/${f3ds}.raw"
    if [[ "$f3ds" == "f3_amplitude" ]]; then
        nx=201; ny=201; nz=51
    else
        nx=191; ny=146; nz=51
    fi

    F3_ERWT3D="$SSD_ERWT3D/${f3ds}_final.erwt3d"
    if [[ ! -f "$F3_ERWT3D" ]]; then
        echo "Converting $f3ds..."
        "$BUILD_DIR/erwt3d_convert" --input "$RAW_F3" --output "$F3_ERWT3D" \
            --nx "$nx" --ny "$ny" --nz "$nz" --threads "$ERWT3D_THREADS" 2>&1 | tee "$ERWT3D_RESULT_ROOT/logs/convert_${f3ds}.log"
    fi

    for axis in x y z; do
        for pattern in random continuous; do
            for run in 1 2 3 4 5; do
                echo "  $f3ds A0_auto_final SSD $axis $pattern run$run"
                "$BENCH_SH" \
                    --input "$F3_ERWT3D" \
                    --dataset "$f3ds" \
                    --format LZ4 \
                    --layout "LZ4+XYZ" \
                    --configuration "A0_auto_final" \
                    --device SSD \
                    --axis "$axis" \
                    --pattern "$pattern" \
                    --run "$run" || echo "[WARN] Failed"
            done
        done
    done
done

# ============================================================
# STEP 10-11: Accuracy
# ============================================================
echo ""
echo "=== STEP 10-11: Accuracy ==="
ACC_DIR="$ERWT3D_RESULT_ROOT/accuracy"
mkdir -p "$ACC_DIR"

# LZ4 bitwise accuracy for all Auto datasets
for pair in "20GB:$AUTO_20GB:$RAW_20GB:801:2405:2501" \
            "f3_amplitude:$SSD_ERWT3D/f3_amplitude_final.erwt3d:$ERWT3D_DATASET_ROOT/f3_amplitude.raw:201:201:51" \
            "f3_similarity:$SSD_ERWT3D/f3_similarity_final.erwt3d:$ERWT3D_DATASET_ROOT/f3_similarity.raw:191:146:51"; do
    IFS=: read -r ds erwt3d_file raw_file nx ny nz <<< "$pair"
    echo "  Accuracy: $ds (LZ4)"
    restored="/tmp/restored_${ds}.raw"
    "$BUILD_DIR/erwt3d_convert" --input "$erwt3d_file" --output "$restored" \
        --nx "$nx" --ny "$ny" --nz "$nz" --threads "$ERWT3D_THREADS" --to-raw 2>&1 | tail -3

    expected_size=$((nx * ny * nz * 4))
    actual_size=$(stat -c%s "$restored" 2>/dev/null || echo 0)

    python3 - "$raw_file" "$restored" "$ds" "$expected_size" "$actual_size" "$ERWT3D_ALGORITHM_COMMIT" "$ACC_DIR" <<'PYEOF'
import hashlib, json, os, sys
raw_path, rest_path, ds, expected, actual, commit, acc_dir = sys.argv[1:]
expected, actual = int(expected), int(actual)

def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(8192), b""):
            h.update(chunk)
    return h.hexdigest()

result = {
    "benchmark_schema_version": "final-1",
    "dataset": ds, "format": "LZ4", "configuration": "A0_auto_final",
    "git_commit": commit, "algorithm_commit": commit,
    "expected_bytes": expected, "actual_bytes": actual,
    "size_match": expected == actual,
}
if expected == actual and os.path.exists(rest_path):
    raw_sha = sha256(raw_path)
    rest_sha = sha256(rest_path)
    result["raw_sha256"] = raw_sha
    result["restored_sha256"] = rest_sha
    result["bitwise_equal"] = (raw_sha == rest_sha)
    result["rmse"] = 0.0 if raw_sha == rest_sha else None
    result["nrmse"] = 0.0 if raw_sha == rest_sha else None
    result["max_abs_error"] = 0.0 if raw_sha == rest_sha else None
    result["max_relative_error"] = 0.0 if raw_sha == rest_sha else None
    result["violations"] = 0
else:
    result["bitwise_equal"] = False
    result["error"] = "size mismatch or file missing"

out = os.path.join(acc_dir, f"accuracy_{ds}_lz4.json")
with open(out, "w") as f:
    json.dump(result, f, indent=2)
print(f"  {ds}: size_match={result['size_match']}, bitwise={result.get('bitwise_equal')}")
PYEOF
    rm -f "$restored"
done

# RZFP formal accuracy for forced RZFP20GB
if [[ -f "$RZFP_20GB" ]]; then
    echo "  Accuracy: 20GB forced RZFP"
    python3 "$VALIDATION_DIR/scripts/stream_accuracy.py" \
        --raw "$RAW_20GB" --erwt3d "$RZFP_20GB" \
        --output "$ACC_DIR/accuracy_20GB_rzfp.json" \
        --threads "$ERWT3D_THREADS" 2>&1 | tail -5 || echo "[WARN] RZFP accuracy script failed"
fi

# ============================================================
# STEP 12: efwi3D fidelity (placeholder)
# ============================================================
echo ""
echo "=== STEP 12: efwi3D Fidelity ==="
echo "[SKIP] efwi3D not available in this environment. Manual step required."

# ============================================================
# STEP 13: Collect results
# ============================================================
echo ""
echo "=== STEP 13: Collect Results ==="
python3 "$VALIDATION_DIR/scripts/collect_final.py" \
    --input-root "$ERWT3D_RESULT_ROOT" \
    --output-root "$VALIDATION_DIR/summaries_final"

# ============================================================
# STEP 14: Generate EXPERIMENT_SUMMARY_FINAL.md
# ============================================================
echo ""
echo "=== STEP 14: Generate Summary ==="
python3 - "$VALIDATION_DIR/summaries_final" "$ERWT3D_ALGORITHM_COMMIT" "$ERWT3D_THREADS" <<'PYEOF'
import csv, json, os, sys
from pathlib import Path

summaries_dir = Path(sys.argv[1])
commit = sys.argv[2]
threads = sys.argv[3]

lines = []
lines.append("# ERWT3D Paper Benchmark — FINAL Experiment Summary\n")
lines.append(f"**Algorithm commit**: `{commit}`")
lines.append(f"**Threads**: {threads}")
lines.append(f"**Cache mode**: cold Linux/WSL guest page-cache")
lines.append("")

# Dataset summary
ds_path = summaries_dir / "dataset_summary.csv"
if ds_path.exists():
    lines.append("## Datasets\n")
    with open(ds_path) as f:
        reader = csv.DictReader(f)
        lines.append("| Dataset | Shape | Raw Size | Auto Format | Package Size | SR | Type |")
        lines.append("|---------|-------|----------|-------------|-------------|-----|------|")
        for r in reader:
            raw_mb = int(r["raw_bytes"]) / 1e6
            pkg_mb = int(r["final_package_bytes"]) / 1e6 if r["final_package_bytes"] != "0" else 0
            lines.append(f"| {r['dataset']} | {r['shape']} | {raw_mb:.1f} MB | {r['auto_format']} | {pkg_mb:.1f} MB | {r['physical_storage_ratio']} | {r['lossless_or_lossy']} |")
    lines.append("")

# Access summary for 20GB
agg_path = summaries_dir / "benchmark_aggregate.csv"
if agg_path.exists():
    lines.append("## 20GB Access Performance\n")
    with open(agg_path) as f:
        reader = csv.DictReader(f)
        lines.append("| Config | Device | Axis | Pattern | Mean(ms) | CV% | Stability |")
        lines.append("|--------|--------|------|---------|----------|-----|-----------|")
        for r in reader:
            if r["dataset"] == "20GB":
                lines.append(f"| {r['configuration']} | {r['device']} | {r['axis']} | {r['pattern']} | {r['mean_ms']} | {r['cv_percent']} | {r['stability']} |")
    lines.append("")

# Accuracy
acc_path = summaries_dir / "accuracy.csv"
if acc_path.exists():
    lines.append("## Accuracy\n")
    with open(acc_path) as f:
        reader = csv.DictReader(f)
        lines.append("| Dataset | Format | Bitwise Equal | RMSE | Max Rel Error | Violations |")
        lines.append("|---------|--------|--------------|------|---------------|------------|")
        for r in reader:
            lines.append(f"| {r.get('dataset','')} | {r.get('format','')} | {r.get('bitwise_equal','')} | {r.get('rmse','')} | {r.get('max_relative_error','')} | {r.get('violations','')} |")
    lines.append("")

lines.append("---\n*Auto-generated by FINAL benchmark suite*\n")
(summaries_dir / "EXPERIMENT_SUMMARY_FINAL.md").write_text("\n".join(lines))
print(f"[OK] EXPERIMENT_SUMMARY_FINAL.md")
PYEOF

# ============================================================
# STEP 15: Audit
# ============================================================
echo ""
echo "=== STEP 15: Final Audit ==="
python3 "$VALIDATION_DIR/scripts/audit_final.py" \
    --input-root "$ERWT3D_RESULT_ROOT" \
    --summaries-root "$VALIDATION_DIR/summaries_final"

echo ""
echo "========================================"
echo "FINAL Suite Complete"
echo "========================================"
