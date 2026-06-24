#!/bin/bash
# 单切片读取延迟测试
# Usage: bench_single.sh [small|big|all]
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/_datasets.sh"

OUT=/mnt/d/CUP/test_hdd/single
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
    local ix=$((NX / 2))
    local iy=$((NY / 2))
    local iz=$((NZ / 2))

    for spec in "X $ix" "Y $iy" "Z $iz"; do
        local axis=$(echo "$spec" | awk '{print $1}')
        local idx=$(echo "$spec" | awk '{print $2}')
        local t0=$(date +%s%N)
        ./build/erwt3d_slice --input "$ERWT" --axis "$axis" --index "$idx" \
            --output "$outdir/${axis}_${idx}.raw"
        local t1=$(date +%s%N)
        local ms=$(( (t1 - t0) / 1000000 ))
        echo "  $axis[$idx]: ${ms}ms"
    done
}

case "$DATASET" in
    small|big) run_one "$DATASET" ;;
    all)        run_one small; run_one big ;;
    *)          echo "Usage: $0 [small|big|all]"; exit 1 ;;
esac
