#!/bin/bash
# 单切片读取延迟测试
set -e

INPUT=${1:-/mnt/d/CUP/cup_3d_small.erwt3d}
OUT=/mnt/d/CUP/test_hdd/single
mkdir -p "$OUT"

echo "Input: $INPUT"
echo "---"

for axis in X Y Z; do
    idx=0
    [ "$axis" = "Y" ] && idx=1200
    [ "$axis" = "Z" ] && idx=1250
    t0=$(date +%s%N)
    ./build/erwt3d_slice --input "$INPUT" --axis "$axis" --index "$idx" --output "$OUT/${axis}_${idx}.raw"
    t1=$(date +%s%N)
    ms=$(( (t1 - t0) / 1000000 ))
    echo "$axis[$idx]: ${ms}ms"
done
