#!/bin/bash
# RAW → ERWT3D 转换 + 验证
# 用法：convert.sh [small|big|all]
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/_datasets.sh"

OUT_BASE=/mnt/d/CUP/test_hdd
DATASET="${1:-small}"
ERWT3D="$SCRIPT_DIR/../build/erwt3d"

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
    $ERWT3D convert input="$RAW" output="$out" \
        nx=$NX ny=$NY nz=$NZ \
        threads=8 memory-limit-mb=4096
    echo "验证中..."
    $ERWT3D verify raw="$RAW" erwt3d="$out" \
        nx=$NX ny=$NY nz=$NZ samples=100000
}

case "$DATASET" in
    small|big) run_one "$DATASET" ;;
    all)        run_one small; run_one big ;;
    *)          echo "用法：$0 [small|big|all]"; exit 1 ;;
esac
