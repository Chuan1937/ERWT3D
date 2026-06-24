#!/bin/bash
# RAW -> ERWT3D 转换
set -e

INPUT=/mnt/d/CUP/cup_3d_small.dat
OUTPUT=/mnt/d/CUP/cup_3d_small.erwt3d

./build/erwt3d_convert --input "$INPUT" --output "$OUTPUT" \
    --nx 801 --ny 2405 --nz 2501 --threads 8 --memory-limit-mb 4096

echo "Convert done: $OUTPUT"
