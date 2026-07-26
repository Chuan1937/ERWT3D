#!/usr/bin/env bash
# SSD cold cache benchmark runner
# Usage:
#   ./tools/run_ssd_cold_benchmark.sh <input> <out_base> <log_dir> [mode]
#
# mode: main (standard reader) | cold (cold executor pread)
# Default: cold
set -euo pipefail

INPUT="${1:-/home/chuan/erwt3d_final_ssd/format/cup_3d_big.erwt3d}"
OUT_BASE="${2:-/tmp/bench_out}"
LOG_DIR="${3:-/tmp/bench_log}"
MODE="${4:-cold}"

POSITIONS="/home/chuan/code/ERWT3D/positions/positions_big.csv"
THREADS=16

mkdir -p "$OUT_BASE" "$LOG_DIR"

clear_guest_cache() {
    sync
    rm -rf "$OUT_BASE"
    mkdir -p "$OUT_BASE"
    sync
    sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null
    echo "=== post-clear memory ==="
    grep -E '^(MemFree|MemAvailable|Buffers|Cached|SReclaimable|Dirty|Writeback):' /proc/meminfo
}

COMMIT=$(git rev-parse HEAD)
TIMESTAMP=$(date '+%Y%m%d_%H%M%S')

if [ "$MODE" = "main" ]; then
    PROFILE_STR="hdd"
    BACKEND="auto"
    LOG_NAME="main_${COMMIT:0:8}_${TIMESTAMP}.log"
elif [ "$MODE" = "cold" ]; then
    PROFILE_STR="ssd"
    BACKEND="pread"
    LOG_NAME="cold_${COMMIT:0:8}_${TIMESTAMP}.log"
else
    echo "Unknown mode: $MODE (use main|cold)"
    exit 1
fi

LOG="$LOG_DIR/$LOG_NAME"

echo "============================================================"
echo "  Benchmark: $MODE"
echo "  Commit:    $COMMIT"
echo "  Threads:   $THREADS"
echo "  Input:     $INPUT"
echo "  Output:    $OUT_BASE"
echo "  Log:       $LOG"
echo "============================================================"

clear_guest_cache

/usr/bin/time -v \
taskset -c 0-15 \
./build/erwt3d_contest \
  --input "$INPUT" \
  --output-dir "$OUT_BASE" \
  --positions-file "$POSITIONS" \
  --threads "$THREADS" \
  --memory-limit-mb 4096 \
  --io-profile "$PROFILE_STR" \
  --ssd-cold-backend "$BACKEND" \
  --ssd-cold-decode-threads "$THREADS" \
  2>&1 | tee "$LOG"

echo "=== Post-run sync ==="
sync

SHA_LOG="$LOG_DIR/sha_${MODE}_${COMMIT:0:8}_${TIMESTAMP}.txt"
sha256sum "$OUT_BASE"/contest_*.dat | sort > "$SHA_LOG"

echo "=== SHA256 sum saved to $SHA_LOG ==="
echo "Done: $(grep -c '.' "$SHA_LOG") outputs"
