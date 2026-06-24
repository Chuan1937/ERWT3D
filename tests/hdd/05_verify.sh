#!/bin/bash
# 正确性验证
set -e

RAW=/mnt/d/CUP/cup_3d_small.dat
ERWT3D=/mnt/d/CUP/cup_3d_small.erwt3d

./build/erwt3d_verify --raw "$RAW" --erwt3d "$ERWT3D" --nx 801 --ny 2405 --nz 2501 --samples 100000

echo "Verify done"
