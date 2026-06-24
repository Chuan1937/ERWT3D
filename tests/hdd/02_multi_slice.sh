#!/bin/bash
# 多切片顺序读取
set -e

INPUT=/mnt/d/CUP/cup_3d_small.erwt3d
OUT=/mnt/d/CUP/test_hdd
mkdir -p "$OUT"

for i in $(seq 1250 1259); do
    echo "Z[$i] ..."
    ./build/erwt3d_slice --input "$INPUT" --axis Z --index "$i" --output "$OUT/z_${i}.raw"
done

echo "Done: $OUT"
