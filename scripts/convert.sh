#!/bin/bash
# RAW → ERWT3D 转换 + 验证
# Usage: convert.sh [small|big|all]
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/_datasets.sh"

OUT_BASE=/mnt/d/CUP/test_hdd
DATASET="${1:-small}"

run_one() {
    local ds="$1"
    resolve_dataset "$ds"
    local out="$OUT_BASE/${ds}_test.erwt3d"
    mkdir -p "$(dirname "$out")"
    echo ""
    echo "=========================================="
    echo "  [$LABEL] $ds"
    echo "    raw:     $RAW"
    echo "    output:  $out"
    echo "=========================================="
    ./build/erwt3d_convert --input "$RAW" --output "$out" \
        --nx $(echo $DIM | awk '{print $1}') \
        --ny $(echo $DIM | awk '{print $2}') \
        --nz $(echo $DIM | awk '{print $3}') \
        --threads 8 --memory-limit-mb 4096
    echo "Verifying..."
    ./build/erwt3d_verify --raw "$RAW" --erwt3d "$out" \
        --nx $(echo $DIM | awk '{print $1}') \
        --ny $(echo $DIM | awk '{print $2}') \
        --nz $(echo $DIM | awk '{print $3}') \
        --samples 100000
}

case "$DATASET" in
    small|big) run_one "$DATASET" ;;
    all)        run_one small; run_one big ;;
    *)          echo "Usage: $0 [small|big|all]"; exit 1 ;;
esac