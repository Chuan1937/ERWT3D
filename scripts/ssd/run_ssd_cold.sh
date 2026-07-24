#!/bin/bash
# run_ssd_cold.sh — Run SSD benchmark with Guest-cold cache (drop_caches)
set -euo pipefail

INPUT_FILE="${1:?Usage: $0 <erwt3d_file> <output_dir> [memory_mb] [threads]}"
OUTPUT_DIR="${2:?Usage: $0 <erwt3d_file> <output_dir> [memory_mb] [threads]}"
MEMORY_MB="${3:-4096}"
THREADS="${4:-8}"

# Verify sudo access before running
if ! sudo -n true 2>/dev/null; then
    echo "ERROR: sudo password required. Run 'sudo -v' first."
    exit 1
fi

for round in 1 2 3; do
    echo "=== Cold Round ${round} ==="
    sync
    if ! sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null; then
        echo "ERROR: drop_caches failed. Cannot guarantee Guest-cold state."
        exit 1
    fi
    sleep 2

    RUN_DIR="${OUTPUT_DIR}/cold_${round}"
    rm -rf "${RUN_DIR}"
    mkdir -p "${RUN_DIR}"

    /usr/bin/time -v -o "${RUN_DIR}/time.txt" \
        ./build/erwt3d_contest \
            --input "${INPUT_FILE}" \
            --output-dir "${RUN_DIR}" \
            --threads "${THREADS}" \
            --memory-limit-mb "${MEMORY_MB}" \
            --io-profile auto 2>&1 | tee "${RUN_DIR}/contest.log"

    echo ""
done

echo "Done. Cache state: Guest-cold (drop_caches), Windows host cache uncontrolled."
