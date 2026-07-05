#!/bin/bash
# 严格比赛口径 benchmark：计时覆盖 open/create -> read/decode/reorder -> write -> close
# 用法：benchmark_contest_strict.sh /path/to/data.erwt3d /path/to/output_dir [/path/to/storage_path]
set -e

if [ $# -lt 2 ] || [ $# -gt 3 ]; then
    echo "Usage: $0 ERWT3D_FILE OUTPUT_DIR [STORAGE_PATH]"
    exit 1
fi

ERWT="$1"
OUT="$2"
STORAGE_PATH="${3:-$ERWT}"

mkdir -p "$OUT"

./build/erwt3d_bench_contest \
    --input "$ERWT" \
    --output-dir "$OUT" \
    --random-count 100 \
    --continuous-count 10 \
    --continuous-start random \
    --timing-mode strict \
    --storage-path "$STORAGE_PATH" \
    --hdd
