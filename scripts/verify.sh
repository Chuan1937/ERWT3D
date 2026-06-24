#!/bin/bash
# 正确性验证：ERWT3D 数据与原始 RAW 逐点比对
set -e

RAW=${1:-/mnt/d/CUP/cup_3d_small.dat}
ERWT3D=${2:-/mnt/d/CUP/cup_3d_small.erwt3d}

./build/erwt3d_verify --raw "$RAW" --erwt3d "$ERWT3D" \
    --nx 801 --ny 2405 --nz 2501 --samples 100000
