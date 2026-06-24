#!/bin/bash
# 完整 HDD 测试 (100 + 10)
set -e

INPUT=/mnt/d/CUP/cup_3d_small.erwt3d
OUT=/mnt/d/CUP/test_hdd/full_hdd
mkdir -p "$OUT"

./build/erwt3d_bench_contest --input "$INPUT" --output-dir "$OUT" \
    --random-count 100 --continuous-count 10 --hdd

echo "Done: $OUT"
