#!/usr/bin/env bash
# Master orchestrator for ERWT3D paper benchmark suite
# Runs all experiments in priority order
#
# Storage layout:
#   SSD (固态): /mnt/f/CUP/  — raw datasets, SSD benchmarks
#   HDD (机械): /mnt/d/      — erwt3d files, HDD benchmarks
#   Results:    validation/   — always in repo (small JSON/CSV)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/utils.sh"

echo "============================================"
echo "  ERWT3D Paper Benchmark Suite"
echo "============================================"
echo "Commit: $(get_git_commit)"
echo "Date:   $(date -Iseconds)"
echo "Host:   $(hostname)"
echo ""
echo "SSD: $SSD_BASE"
echo "HDD: $HDD_BASE"
echo ""

# --- Available datasets ---
echo "=== Available Datasets ==="
echo ""
echo "[SSD raw files]"
ls -lh "$SSD_DATASET_DIR"/*.dat 2>/dev/null || echo "  (none)"
echo ""
echo "[HDD erwt3d files]"
ls -lh "$HDD_DATASET_DIR"/*.erwt3d 2>/dev/null || echo "  (none)"
echo ""

# --- Pre-flight checks ---
echo "[1/8] Pre-flight checks..."
check_binary erwt3d_bench_contest
check_binary erwt3d_convert
check_binary erwt3d_verify
check_binary erwt3d_info

# Verify key datasets exist
SSD_SMALL="$SSD_DATASET_DIR/cup_3d_small.dat"
SSD_BIG="$SSD_DATASET_DIR/cup_3d_big.dat"
HDD_SMALL="$HDD_DATASET_DIR/small.erwt3d"
HDD_BIG="$HDD_DATASET_DIR/big.erwt3d"

for f in "$SSD_SMALL" "$SSD_BIG"; do
    if [[ ! -f "$f" ]]; then
        echo "[WARN] Missing SSD dataset: $f"
    fi
done
for f in "$HDD_SMALL" "$HDD_BIG"; do
    if [[ ! -f "$f" ]]; then
        echo "[WARN] Missing HDD dataset: $f"
    fi
done
echo "[OK] Pre-flight checks done"
echo ""

# --- Environment audit ---
echo "[2/8] Environment audit..."
bash "$SCRIPT_DIR/audit_environment.sh"
echo ""

# --- Experiment 1: Compression & Storage ---
echo "[3/8] Experiment 1: Compression & Storage..."
echo "  Converting raw → erwt3d (20GB small dataset)..."
if [[ -f "$SSD_SMALL" ]]; then
    mkdir -p "$SSD_WORK_DIR"
    # Compression test on SSD (fast I/O for conversion timing)
    bash "$SCRIPT_DIR/run_compression.sh" \
        "cup_3d_small" "$SSD_SMALL" 801 2405 2501 5
fi
echo ""

# --- Experiment 2: Write Performance (same as compression) ---
echo "[4/8] Experiment 2: Write Performance (included in compression experiment)"
echo ""

# --- Experiment 3: Reconstruction Accuracy ---
echo "[5/8] Experiment 3: Reconstruction Accuracy..."
if [[ -f "$SSD_SMALL" ]]; then
    bash "$SCRIPT_DIR/run_accuracy.sh" \
        "cup_3d_small" "$SSD_SMALL" 801 2405 2501
fi
echo ""

# --- Experiment 4+5: Random + Continuous Read (SSD) ---
echo "[6/8] Experiments 4+5: Read Performance (SSD)..."
if [[ -f "$HDD_SMALL" ]]; then
    IO_DEVICE=SSD bash "$SCRIPT_DIR/run_read_bench.sh" \
        "$HDD_SMALL" "cup_3d_small" "20GB" 5 "--cold"
fi
echo ""

# --- Experiment 4+5: Random + Continuous Read (HDD) ---
echo "[6b/8] Experiments 4+5: Read Performance (HDD)..."
if [[ -f "$HDD_BIG" ]]; then
    IO_DEVICE=HDD bash "$SCRIPT_DIR/run_read_bench.sh" \
        "$HDD_BIG" "cup_3d_big" "50GB" 5 "--cold"
fi
echo ""

# --- Experiment 6: Ablation ---
echo "[7/8] Experiment 9: Ablation Study..."
if [[ -f "$HDD_SMALL" ]]; then
    bash "$SCRIPT_DIR/run_ablation.sh" \
        "$HDD_SMALL" "cup_3d_small" "20GB"
fi
echo ""

# --- Experiment 11: Error threshold sensitivity ---
echo "[8/8] Experiment 11: Error Threshold Sensitivity..."
if [[ -f "$SSD_SMALL" ]]; then
    bash "$SCRIPT_DIR/run_error_sensitivity.sh" \
        "$SSD_SMALL" "cup_3d_small" 801 2405 2501
fi
echo ""

echo "============================================"
echo "  Benchmark suite complete"
echo "============================================"
echo ""
echo "Results: $RESULTS_BASE/"
echo "Logs:    $LOGS_BASE/"
echo ""
echo "Collect: python3 $SCRIPT_DIR/collect_results.py"
