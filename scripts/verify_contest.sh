#!/bin/bash
# 赛题口径正确性验证：默认使用相对误差阈值 1e-3。
# 用法：verify_contest.sh RAW_FILE ERWT3D_FILE NX NY NZ
set -e

if [ $# -ne 5 ]; then
    echo "Usage: $0 RAW_FILE ERWT3D_FILE NX NY NZ"
    exit 1
fi

RAW="$1"
ERWT="$2"
NX="$3"
NY="$4"
NZ="$5"

./build/erwt3d_verify \
    --raw "$RAW" \
    --erwt3d "$ERWT" \
    --nx "$NX" \
    --ny "$NY" \
    --nz "$NZ" \
    --samples 100000 \
    --rel-tol 1e-3
