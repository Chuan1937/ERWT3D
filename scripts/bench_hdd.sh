#!/bin/bash
# bench_hdd.sh - HDD optimization benchmark
# Usage: ./scripts/bench_hdd.sh [erwt3d_file] [output_dir]
#
# If no erwt3d_file given, generates 200x300x400 test data automatically.
# Runs 8 HDD-tuned configurations and outputs comparison table.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD="$PROJECT_DIR/build"

# Parameters
SEED=20260511
RANDOM_COUNT=${RANDOM_COUNT:-100}
CONTINUOUS_COUNT=${CONTINUOUS_COUNT:-10}
MEMORY_LIMIT=${MEMORY_LIMIT:-2048}

# Auto-detect threads (cap at 8)
HW_THREADS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
THREADS=$((HW_THREADS > 8 ? 8 : HW_THREADS))

# Input file
INPUT="${1:-}"
OUTPUT_DIR="${2:-$PROJECT_DIR/bench_hdd_$(date +%Y%m%d_%H%M%S)}"

# If no input, generate test data
if [ -z "$INPUT" ]; then
    NX=200; NY=300; NZ=400
    RAW_FILE="/tmp/erwt3d_test_${NX}x${NY}x${NZ}.raw"
    ERWT3D_FILE="/tmp/erwt3d_test_${NX}x${NY}x${NZ}.erwt3d"

    if [ ! -f "$ERWT3D_FILE" ]; then
        echo "[1/2] Generating test data ${NX}x${NY}x${NZ}..."
        "$BUILD/gen_test_data" --output "$RAW_FILE" --nx $NX --ny $NY --nz $NZ --seed 42
        echo "[2/2] Converting to erwt3d format..."
        "$BUILD/erwt3d_convert" --input "$RAW_FILE" --output "$ERWT3D_FILE" \
            --nx $NX --ny $NY --nz $NZ --threads $THREADS
    else
        echo "[skip] Test data exists: $ERWT3D_FILE"
    fi
    INPUT="$ERWT3D_FILE"
fi

if [ ! -f "$INPUT" ]; then
    echo "Error: Input file not found: $INPUT"
    exit 1
fi

# Build if needed
if [ ! -f "$BUILD/erwt3d_bench_contest" ]; then
    echo "Building..."
    cmake -S "$PROJECT_DIR" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$BUILD" -j
fi

echo "============================================================"
echo "  HDD Benchmark Suite"
echo "============================================================"
echo "  Input:    $INPUT"
echo "  Output:   $OUTPUT_DIR"
echo "  Threads:  $THREADS"
echo "  Random:   $RANDOM_COUNT/axis"
echo "  Continu:  $CONTINUOUS_COUNT/axis"
echo "  MemLimit: $MEMORY_LIMIT MB"
echo "  Seed:     $SEED"
echo "============================================================"

mkdir -p "$OUTPUT_DIR"

drop_cache() {
    if [ "$(id -u)" -eq 0 ]; then
        echo 3 > /proc/sys/vm/drop_caches 2>/dev/null && echo "[cache] dropped" || true
    elif command -v sudo &>/dev/null && sudo -n true 2>/dev/null; then
        sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null && echo "[cache] dropped" || true
    else
        echo "[cache] skipped"
    fi
}

COMMON_ARGS=(
    --input "$INPUT"
    --random-count "$RANDOM_COUNT"
    --continuous-count "$CONTINUOUS_COUNT"
    --memory-limit-mb "$MEMORY_LIMIT"
    --seed "$SEED"
)

run_cfg() {
    local name="$1"; shift
    local dir="$OUTPUT_DIR/$name"
    mkdir -p "$dir"

    echo ""
    echo ">>> $name"

    drop_cache

    "$BUILD/erwt3d_bench_contest" \
        --output-dir "$dir" \
        "${COMMON_ARGS[@]}" \
        "$@" \
        2>&1 | tee "$dir/run.log"
}

# ---- Run configurations ----

# C1: Baseline - extent-based pread, single thread
run_cfg "c01_pread_t1" \
    --threads 1 --io-backend pread

# C2: Superblock serial, single thread
run_cfg "c02_sb_serial_t1" \
    --threads 1 --io-backend sb --sb-parallel-mode serial

# C3: Superblock parallel-read, multi thread
run_cfg "c03_sb_parread_t${THREADS}" \
    --threads "$THREADS" --io-backend sb --sb-parallel-mode parallel-read

# C4: + file-offset sort (minimize HDD seeks)
run_cfg "c04_offset_t${THREADS}" \
    --threads "$THREADS" --io-backend sb --sb-parallel-mode parallel-read \
    --sb-task-order file-offset

# C5: + HDD read window (merge nearby reads into large windows)
run_cfg "c05_hddwin_t${THREADS}" \
    --threads "$THREADS" --io-backend sb --sb-parallel-mode parallel-read \
    --sb-task-order file-offset \
    --sb-read-mode hdd-read-window \
    --hdd-read-window-bytes 33554432 --hdd-max-gap-bytes 262144

# C6: RunBatch (merge contiguous superblocks into single pread)
run_cfg "c06_runbatch_t${THREADS}" \
    --threads "$THREADS" --io-backend sb --sb-parallel-mode parallel-read \
    --sb-read-mode run-batch

# C7: Pinned threads
run_cfg "c07_pinned_t${THREADS}" \
    --threads "$THREADS" --io-backend sb --sb-parallel-mode parallel-read \
    --sb-task-order file-offset --pin-threads

# C8: Best combo + cache (warm run)
run_cfg "c08_cached_t${THREADS}" \
    --threads "$THREADS" --io-backend sb --sb-parallel-mode parallel-read \
    --sb-task-order file-offset --cache-mb 256

# ---- Summary ----
echo ""
echo "============================================================"
echo "  RESULTS"
echo "============================================================"
echo ""
printf "%-24s | %10s | %7s | %5s\n" "Config" "T_composite" "Storage" "Store"
printf "%-24s-+-%10s-+-%7s-+-%5s\n" "------------------------" "----------" "-------" "-----"

best_t=""
best_name=""
for dir in "$OUTPUT_DIR"/c*/; do
    [ -d "$dir" ] || continue
    name=$(basename "$dir")
    sf="$dir/contest_score.csv"
    if [ -f "$sf" ]; then
        t=$(grep "^T_composite_ms," "$sf" | cut -d, -f2)
        s=$(grep "^storage_ratio," "$sf" | cut -d, -f2)
        ss=$(grep "^storage_score," "$sf" | cut -d, -f2)
        printf "%-24s | %7s ms | %6sx | %s/20\n" "$name" "$t" "$s" "$ss"
        if [ -z "$best_t" ] || awk "BEGIN{exit !($t < $best_t)}"; then
            best_t="$t"
            best_name="$name"
        fi
    fi
done

echo ""
echo "  Best: $best_name ($best_t ms)"
echo ""
echo "  Per-axis breakdown (best config):"
sf="$OUTPUT_DIR/$best_name/contest_score.csv"
if [ -f "$sf" ]; then
    for axis in x y z; do
        tr=$(grep "^T_${axis}_random_ms," "$sf" | cut -d, -f2)
        tc=$(grep "^T_${axis}_continuous_ms," "$sf" | cut -d, -f2)
        printf "    %s: random=%7s ms  continuous=%7s ms\n" "$axis" "$tr" "$tc"
    done
fi

echo ""
echo "  Results: $OUTPUT_DIR/"
echo "============================================================"
