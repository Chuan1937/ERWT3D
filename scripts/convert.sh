#!/bin/bash
# RAW → ERWT3D 转换测试
set -e

INPUT=${1:-/mnt/d/CUP/cup_3d_small.dat}
OUTPUT=${2:-/mnt/d/CUP/test_hdd/cup_3d_small_test.erwt3d}

mkdir -p "$(dirname "$OUTPUT")"

./build/erwt3d_convert --input "$INPUT" --output "$OUTPUT" \
    --nx 801 --ny 2405 --nz 2501 --threads 8 --memory-limit-mb 4096

echo "Verifying..."
./build/erwt3d_verify --raw "$INPUT" --erwt3d "$OUTPUT" \
    --nx 801 --ny 2405 --nz 2501 --samples 100000
