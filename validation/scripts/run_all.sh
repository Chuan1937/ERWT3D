#!/usr/bin/env bash
# Master orchestrator for ERWT3D paper benchmark suite
# Runs all experiments in priority order
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

# --- Pre-flight checks ---
echo "[1/8] Pre-flight checks..."
check_binary erwt3d_contest
check_binary erwt3d_convert
check_binary erwt3d_verify
check_binary erwt3d_info

# Check for workloads
for f in random_x.txt random_y.txt random_z.txt continuous_x.txt continuous_y.txt continuous_z.txt; do
    for ds in 20GB 50GB; do
        wf="$VALIDATION_DIR/workloads/$ds/$f"
        if [[ ! -f "$wf" ]]; then
            echo "[ERROR] Missing workload: $wf"
            echo "        Run: python3 $SCRIPT_DIR/generate_workloads.py"
            exit 1
        fi
    done
done
echo "[OK] All pre-flight checks passed"
echo ""

# --- Environment audit ---
echo "[2/8] Environment audit..."
bash "$SCRIPT_DIR/audit_environment.sh"
echo ""

# --- Experiment 1: Compression & Storage ---
echo "[3/8] Experiment 1: Compression & Storage..."
echo "TODO: Implement compression benchmark for Raw/LZ4/ZFP/HDF5/ERWT3D"
echo ""

# --- Experiment 2: Write Performance ---
echo "[4/8] Experiment 2: Write Performance..."
echo "TODO: Implement encode/throughput benchmark"
echo ""

# --- Experiment 3: Reconstruction Accuracy ---
echo "[5/8] Experiment 3: Reconstruction Accuracy..."
echo "TODO: Implement accuracy verification"
echo ""

# --- Experiment 4 & 5: Random + Continuous Read ---
echo "[6/8] Experiments 4+5: Read Performance..."

METHODS=("erwt3d")
DATASETS=("datasetA")
SIZES=("20GB")
AXES=("x" "y" "z")
PATTERNS=("random" "continuous")
N_RUNS=5

for method in "${METHODS[@]}"; do
    for ds in "${DATASETS[@]}"; do
        for size in "${SIZES[@]}"; do
            for axis in "${AXES[@]}"; do
                for pattern in "${PATTERNS[@]}"; do
                    for run in $(seq 1 $N_RUNS); do
                        echo "  Running: $method / $ds / $size / $axis / $pattern / run $run"
                        # bash "$SCRIPT_DIR/run_benchmark.sh" \
                        #     "$method" "$ds" "$size" "$axis" "$pattern" "$run"
                    done
                done
            done
        done
    done
done
echo ""

# --- Experiment 6: HDD vs SSD ---
echo "[7/8] Experiment 6: HDD vs SSD..."
echo "TODO: Set IO_DEVICE=HDD and IO_DEVICE=SSD, re-run experiments 4+5"
echo ""

# --- Experiment 7-12: Extended experiments ---
echo "[8/8] Extended experiments (scalabiltiy, ablation, fidelity)..."
echo "TODO: Implement remaining experiments"
echo ""

echo "============================================"
echo "  Benchmark suite complete"
echo "============================================"
echo ""
echo "Results in: $VALIDATION_DIR/raw_results/"
echo "Logs in:    $VALIDATION_DIR/logs/"
echo ""
echo "Next: python3 $SCRIPT_DIR/collect_results.py"
