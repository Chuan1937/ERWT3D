#!/bin/bash
# 赛题标准 benchmark（100 random + 10 continuous）
# Usage: benchmark.sh [small|big|all]
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/_datasets.sh"

OUT=/mnt/d/CUP/test_hdd/bench
mkdir -p "$OUT"

DATASET="${1:-small}"

run_one() {
    local ds="$1"
    resolve_dataset "$ds"
    local outdir="$OUT/$ds"
    mkdir -p "$outdir"
    echo ""
    echo "=========================================="
    echo "  [$LABEL] $ds -> $outdir"
    echo "=========================================="
    ./build/erwt3d_bench_contest \
        --input "$ERWT" --output-dir "$outdir" \
        --random-count 100 --continuous-count 10 --hdd
}

case "$DATASET" in
    small|big) run_one "$DATASET" ;;
    all)        run_one small; run_one big ;;
    *)          echo "Usage: $0 [small|big|all]"; exit 1 ;;
esac
