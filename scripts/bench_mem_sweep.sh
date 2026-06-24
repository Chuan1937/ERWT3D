#!/bin/bash
# 内存限制扫描（2/4/8/16/32/64 GB × 20GB/50GB）
set -e

SMALL=/mnt/d/CUP/cup_3d_small.erwt3d
BIG=/mnt/d/CUP/cup_3d_big.erwt3d
OUT=/mnt/d/CUP/test_hdd/mem_sweep
BIN=./build/erwt3d_bench_contest
RESULT="$OUT/results.csv"
mkdir -p "$OUT"

echo "dataset,mem_mb,x_random,y_random,z_random,x_cont,y_cont,z_cont,T_composite" > "$RESULT"

TOTAL=12
DONE=0

run_one() {
    local ds="$1" label="$2" mem="$3"
    DONE=$((DONE + 1))
    local od="$OUT/${label}_${mem}gb"
    mkdir -p "$od"
    echo ""
    echo "========================================"
    echo "  [$DONE/$TOTAL] $label  memory=${mem}GB"
    echo "========================================"
    local out
    out=$("$BIN" --input "$ds" --output-dir "$od" \
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
        echo "$label,$mem,$xr,$yr,$zr,$xc,$yc,$zc,$tc" >> "$RESULT"
    fi
    rm -rf "$od"
}

for mem in 2 4 8 16 32 64; do run_one "$SMALL" "20GB" "$mem"; done
for mem in 2 4 8 16 32 64; do run_one "$BIG"   "50GB" "$mem"; done

echo ""
echo "=== RESULTS ==="
column -t -s',' "$RESULT"
