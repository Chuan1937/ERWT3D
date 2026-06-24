#!/bin/bash
# 赛题标准 benchmark（100 random + 10 continuous）
set -e

INPUT=${1:-/mnt/d/CUP/cup_3d_small.erwt3d}
OUT=/mnt/d/CUP/test_hdd/bench
mkdir -p "$OUT"

./build/erwt3d_bench_contest --input "$INPUT" --output-dir "$OUT" \
    --random-count 100 --continuous-count 10 --hdd
