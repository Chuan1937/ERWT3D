#!/bin/bash
# 正确性验证：ERWT3D 数据与原始 RAW 逐点比对
# Usage: verify.sh [small|big|all]
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/_datasets.sh"

DATASET="${1:-small}"

run_one() {
    local ds="$1"
    resolve_dataset "$ds"
    echo ""
    echo "=========================================="
    echo "  [$LABEL] $ds"
    echo "    raw:     $RAW"
    echo "    erwt3d:  $ERWT"
    echo "=========================================="
    ./build/erwt3d_verify --raw "$RAW" --erwt3d "$ERWT" \
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