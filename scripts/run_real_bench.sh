#!/bin/bash
# ERWT3D real-data benchmark script
# Usage: scripts/run_real_bench.sh <raw_file> <nx> <ny> <nz> [output_prefix]
# Example: scripts/run_real_bench.sh data.raw 801 2405 2501 benchmarks/real

set -euo pipefail

RAW="${1:?Usage: $0 <raw_file> <nx> <ny> <nz> [output_prefix]}"
NX="${2:?}"
NY="${3:?}"
NZ="${4:?}"
PREFIX="${5:-benchmarks/real}"
BUILD="${BUILD_DIR:-./build}"
SEED=20260511

echo "=== ERWT3D Real Data Benchmark ==="
echo "Raw: $RAW"
echo "Dimensions: ${NX} x ${NY} x ${NZ}"
echo "Output prefix: $PREFIX"
echo "Build dir: $BUILD"
echo ""

# Step 1: Convert raw to ERWT3D
ERWT3D="${PREFIX}.erwt3d"
echo "--- Step 1: Convert to ERWT3D ---"
if [ ! -f "$ERWT3D" ]; then
    "$BUILD/erwt3d_convert" \
        --input "$RAW" \
        --output "$ERWT3D" \
        --nx "$NX" --ny "$NY" --nz "$NZ" \
        --threads 8 \
        --memory-limit-mb 4096
else
    echo "  $ERWT3D exists, skipping"
fi

# Step 2: File info
echo ""
echo "--- Step 2: File info ---"
"$BUILD/erwt3d_info" "$ERWT3D"

# Step 3: Verify correctness
echo ""
echo "--- Step 3: Verify (100k samples) ---"
"$BUILD/erwt3d_verify" \
    --raw "$RAW" \
    --erwt3d "$ERWT3D" \
    --nx "$NX" --ny "$NY" --nz "$NZ" \
    --samples 100000

# Step 4: ERWT3D benchmark with cache
echo ""
echo "--- Step 4: ERWT3D benchmark (t8, cache 512MB) ---"
mkdir -p "${PREFIX}_erwt3d_cache512"
"$BUILD/erwt3d_bench" \
    --input "$ERWT3D" \
    --output-dir "${PREFIX}_erwt3d_cache512" \
    --random-count 100 \
    --continuous-count 10 \
    --threads 8 \
    --memory-limit-mb 4096 \
    --cache-mb 512 \
    --seed "$SEED"

# Step 5: ERWT3D benchmark without cache
echo ""
echo "--- Step 5: ERWT3D benchmark (t8, cache 0) ---"
mkdir -p "${PREFIX}_erwt3d_cache0"
"$BUILD/erwt3d_bench" \
    --input "$ERWT3D" \
    --output-dir "${PREFIX}_erwt3d_cache0" \
    --random-count 100 \
    --continuous-count 10 \
    --threads 8 \
    --memory-limit-mb 4096 \
    --cache-mb 0 \
    --seed "$SEED"

# Step 6: Raw baseline
echo ""
echo "--- Step 6: Raw baseline ---"
mkdir -p "${PREFIX}_raw_baseline"
"$BUILD/erwt3d_bench_raw" \
    --input "$RAW" \
    --nx "$NX" --ny "$NY" --nz "$NZ" \
    --output-dir "${PREFIX}_raw_baseline" \
    --random-count 100 \
    --continuous-count 10 \
    --seed "$SEED"

# Step 7: Thread scaling
echo ""
echo "--- Step 7: Thread scaling (t1, t2, t4, t8) ---"
for t in 1 2 4 8; do
    echo "  Threads=$t..."
    mkdir -p "${PREFIX}_erwt3d_t${t}"
    "$BUILD/erwt3d_bench" \
        --input "$ERWT3D" \
        --output-dir "${PREFIX}_erwt3d_t${t}" \
        --random-count 20 \
        --continuous-count 5 \
        --threads "$t" \
        --memory-limit-mb 4096 \
        --cache-mb 0 \
        --seed "$SEED" 2>&1 | tee "${PREFIX}_erwt3d_t${t}/run.log" | grep -E "T_total|T_random|T_cont" || true
    echo "  Saved to ${PREFIX}_erwt3d_t${t}/bench_result.csv"
done

echo ""
echo "=== Benchmark complete ==="
echo "Results: ${PREFIX}_*/"