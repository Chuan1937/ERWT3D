#!/bin/bash
# Batch vs 逐切片对比
set -e

INPUT=/mnt/d/CUP/cup_3d_small.erwt3d
OUT=/mnt/d/CUP/test_hdd/batch_vs_single
mkdir -p "$OUT"

./build/erwt3d_bench_contest --input "$INPUT" --output-dir "$OUT" \
    --random-count 10 --continuous-count 5 --hdd

echo "Done: $OUT"
