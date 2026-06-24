#!/bin/bash
# 单切片读取延迟
set -e

INPUT=/mnt/d/CUP/cup_3d_small.erwt3d
OUT=/mnt/d/CUP/test_hdd
mkdir -p "$OUT"

echo "=== X slice ==="
./build/erwt3d_slice --input "$INPUT" --axis X --index 400 --output "$OUT/x.raw"
echo "=== Y slice ==="
./build/erwt3d_slice --input "$INPUT" --axis Y --index 1200 --output "$OUT/y.raw"
echo "=== Z slice ==="
./build/erwt3d_slice --input "$INPUT" --axis Z --index 1250 --output "$OUT/z.raw"

echo "Done: $OUT"
