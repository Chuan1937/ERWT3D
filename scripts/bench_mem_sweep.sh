#!/bin/bash
# 内存限制扫描（2/4/8/16/32/64 GB × 20GB/50GB）
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/_datasets.sh"

OUT=/mnt/d/CUP/test_hdd/mem_sweep
BIN=./build/erwt3d_bench_contest
RESULT="$OUT/results.csv"
mkdir -p "$OUT"

echo "dataset,mem_mb,x_random,y_random,z_random,x_cont,y_cont,z_cont,T_composite" > "$RESULT"

TOTAL=12
DONE=0

run_one() {
    local ds="$1" mem="$2"
    resolve_dataset "$ds"
    DONE=$((DONE + 1))
    local od="$OUT/${LABEL}_${mem}gb"
    mkdir -p "$od"
    echo ""
    echo "========================================"
    echo "  [$DONE/$TOTAL] $LABEL  memory=${mem}GB"
    echo "========================================"
    local out
    out=$("$BIN" --input "$ERWT" --output-dir "$od" \
        --random-count 100 --continuous-count 10 \
        --hdd --memory-limit-mb "$((mem * 1024))" --seed 20260511 2>&1)
    echo "$out" | grep -E '^\s*\[[1-6]/6\]'
    echo "$out" | grep 'T_composite = total'
    local csv="$od/contest_summary.csv"
    if [ -f "$csv" ]; then
        local xr=$(sed -n '2p' "$csv" | cut -d',' -f5)
        local yr=$(sed -n '3p' "$csv" | cut -d',' -f5)
        local zr=$(sed -n '4p' "$csv" | cut -d',' -f5)
        local xc=$(sed -n '5p' "$csv" | cut -d',' -f5)
        local yc=$(sed -n '6p' "$csv" | cut -d',' -f5)
        local zc=$(sed -n '7p' "$csv" | cut -d',' -f5)
        local tc=$(echo "$out" | grep 'T_composite = total' | grep -oP '[\d.]+(?=s\b)')
        echo "$LABEL,$mem,$xr,$yr,$zr,$xc,$yc,$zc,$tc" >> "$RESULT"
    fi
    rm -rf "$od"
}

for ds in small big; do
    for mem in 2 4 8 16 32 64; do
        run_one "$ds" "$mem"
    done
done

echo ""
echo "=== RESULTS ==="
column -t -s',' "$RESULT"