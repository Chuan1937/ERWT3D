#!/bin/bash
set -euo pipefail
# Usage: ./probe_dataset.sh /mnt/g/CUP/cup_3d_small.dat 801 2405 2501 small

RAW="$1"; NX="$2"; NY="$3"; NZ="$4"; LABEL="${5:-dataset}"
BUILD="${BUILD_DIR:-build}"
OUTDIR="${OUTPUT_DIR:-/mnt/g/ERWT3D_validation/results/planner}"
mkdir -p "$OUTDIR"

echo "=== Probing $LABEL ($NX x $NY x $NZ) ==="
echo "Raw: $RAW"
echo ""

# LZ4 compression estimate (sample first 64 X-slabs)
echo "--- LZ4 Probing ---"
./"$BUILD"/erwt3d_precompute_x \
    --raw "$RAW" --erwt3d /dev/null \
    --nx "$NX" --ny "$NY" --nz "$NZ" \
    --mode sidecar --stride 1 --storage-budget 5.0 2>&1 | \
    grep -E "compression ratio|Projected|Stride|Planes|creating" || true

echo ""
echo "--- RZFP Probing ---"
./"$BUILD"/erwt3d_rzfp_probe \
    --raw "$RAW" --nx "$NX" --ny "$NY" --nz "$NZ" \
    --max-leaves 100000 2>&1 | tail -20

echo ""
echo "Results saved to: $OUTDIR/"
