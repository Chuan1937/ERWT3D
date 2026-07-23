#!/bin/bash
# run_ssd_cold.sh — Run SSD benchmark with cold cache (drop_caches)

set -euo pipefail

INPUT_FILE="${1:?Usage: $0 <erwt3d_file> <output_dir>}"
OUTPUT_DIR="${2:?Usage: $0 <erwt3d_file> <output_dir>}"

for round in 1 2 3; do
    echo "=== Cold Round ${round} ==="
    sync
    sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null || true
    sleep 2
    ./build/erwt3d_contest \
        --input "${INPUT_FILE}" \
        --output-dir "${OUTPUT_DIR}/cold_${round}" \
        --threads 8 \
        --io-profile wsl-ssd
done

echo "Done."
