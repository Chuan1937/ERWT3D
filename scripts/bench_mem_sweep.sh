#!/bin/bash
set -e

SMALL="/mnt/d/CUP/cup_3d_small.erwt3d"
BIG="/mnt/d/CUP/cup_3d_big.erwt3d"
OUTDIR="/mnt/d/CUP/bench_mem_sweep"
BIN="./build/erwt3d_bench_contest"
RESULT="$OUTDIR/results.csv"

mkdir -p "$OUTDIR"
echo "dataset,mem_mb,x_random,y_random,z_random,x_cont,y_cont,z_cont,T_composite" > "$RESULT"

TOTAL=12
DONE=0

run_one() {
    local ds="$1" label="$2" mem="$3"
    DONE=$((DONE + 1))
    local od="$OUTDIR/${label}_${mem}gb"
    mkdir -p "$od"
    echo "" >&2
    echo "========================================" >&2
    echo "  [$DONE/$TOTAL] $label  memory=${mem}GB" >&2
    echo "========================================" >&2
    local out
    out=$("$BIN" --input "$ds" --output-dir "$od" \
        --random-count 100 --continuous-count 10 \
        --hdd --memory-limit-mb "$((mem * 1024))" --seed 20260511 2>&1)
    # Print only group lines for progress
    echo "$out" | grep -E '^\s*\[[1-6]/6\]' >&2
    echo "$out" | grep 'T_composite = total' >&2
    local csv="$od/contest_summary.csv"
    if [ -f "$csv" ]; then
        local xr=$(sed -n '2p' "$csv" | cut -d',' -f5)
        local yr=$(sed -n '3p' "$csv" | cut -d',' -f5)
        local zr=$(sed -n '4p' "$csv" | cut -d',' -f5)
        local xc=$(sed -n '5p' "$csv" | cut -d',' -f5)
        local yc=$(sed -n '6p' "$csv" | cut -d',' -f5)
        local zc=$(sed -n '7p' "$csv" | cut -d',' -f5)
        local tc=$(echo "$out" | grep 'T_composite = total' | grep -oP '[\d.]+(?=s\b)')
        echo "$label,$mem,$xr,$yr,$zr,$xc,$yc,$zc,$tc" >> "$RESULT"
    fi
    rm -rf "$od"
}

run_one "$SMALL" "20GB" 2
run_one "$SMALL" "20GB" 4
run_one "$SMALL" "20GB" 8
run_one "$SMALL" "20GB" 16
run_one "$SMALL" "20GB" 32
run_one "$SMALL" "20GB" 64

run_one "$BIG" "50GB" 2
run_one "$BIG" "50GB" 4
run_one "$BIG" "50GB" 8
run_one "$BIG" "50GB" 16
run_one "$BIG" "50GB" 32
run_one "$BIG" "50GB" 64

echo "" >&2
echo "=== ALL DONE ===" >&2
column -t -s',' "$RESULT"
