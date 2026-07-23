#!/bin/bash
# run_ssd_matrix.sh — Run SSD benchmark matrix (threads, extents, buffer sizes)

set -euo pipefail

INPUT_FILE="${1:?Usage: $0 <erwt3d_file> <output_dir> [threads] [memory_mb]}"
OUTPUT_DIR="${2:?Usage: $0 <erwt3d_file> <output_dir> [threads] [memory_mb]}"
THREADS="${3:-8}"
MEMORY_MB="${4:-8192}"

echo "=== SSD Benchmark Matrix ==="
echo "Input:  ${INPUT_FILE}"
echo "Output: ${OUTPUT_DIR}"
echo "Threads: ${THREADS}"
echo "Memory:  ${MEMORY_MB} MB"
echo

for READ_THREADS in 2 4 8; do
    for WINDOW_MB in 1 4 16; do
        for GAP_KB in 0 64 1024; do
            echo "--- r${READ_THREADS}_w${WINDOW_MB}m_g${GAP_KB}k ---"
            ./build/erwt3d_bench_ssd
        done
    done
done

echo "Done."
