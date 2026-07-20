#!/bin/bash
set -uo pipefail

# ============================================================
# ERWT3D Full-Scale Benchmark Script
# Max CPU (16 threads) + Max Memory (48GB for convert/bench)
# Sequential per-size, clean up after each size
# ============================================================

BUILD=/home/chuan/code/ERWT3D/build-final
GEN=$BUILD/gen_fast_data
CONVERT=$BUILD/erwt3d_convert
CONVERT_RZFP=$BUILD/erwt3d_convert_rzfp
VERIFY=$BUILD/erwt3d_verify
VERIFY_RZFP=$BUILD/erwt3d_verify_rzfp
BENCH=$BUILD/erwt3d_bench_contest
INFO=$BUILD/erwt3d_info

BASEDIR=/mnt/g/erwt3d_bench
RESULTSDIR=/home/chuan/code/ERWT3D/bench_results
THREADS=16
CONVERT_MEM=49152
SEED=20260511

mkdir -p "$BASEDIR" "$RESULTSDIR"

RUNID=$(date +%Y%m%d_%H%M%S)
LOGFILE="$RESULTSDIR/run_${RUNID}.log"
CSVFILE="$RESULTSDIR/results_${RUNID}.csv"

echo "=== ERWT3D Full-Scale Benchmark ===" | tee "$LOGFILE"
echo "Run ID: $RUNID" | tee -a "$LOGFILE"
echo "Threads: $THREADS | Convert mem: ${CONVERT_MEM}MB" | tee -a "$LOGFILE"
echo "" | tee -a "$LOGFILE"

echo "label,gb_target,nx,ny,nz,actual_gb,format,storage_ratio,mem_mb,window_mb,T_composite,T_xr,T_yr,T_zr,T_xc,T_yc,T_zc,rss_gb,convert_s,verify_ok,notes" > "$CSVFILE"

log() { echo "[$(date '+%H:%M:%S')] $*" | tee -a "$LOGFILE"; }

check_space() {
    local need_gb=$1
    local avail_kb=$(df /mnt/g/ | tail -1 | awk '{print $4}')
    local avail_gb=$((avail_kb / 1024 / 1024))
    log "Available: ${avail_gb} GB, need ${need_gb} GB"
    [ "$avail_gb" -ge "$need_gb" ]
}

# Grid dimensions (Nx:Ny:Nz ≈ 2:2.2:3, 32k+1/9/23)
NX[5]=929;   NY[5]=1033;  NZ[5]=1399
NX[10]=1185; NY[10]=1289; NZ[10]=1751
NX[30]=1697; NY[30]=1865; NZ[30]=2551
NX[40]=1857; NY[40]=2057; NZ[40]=2807
NX[70]=2241; NY[70]=2473; NZ[70]=3383
NX[100]=2529;NY[100]=2793;NZ[100]=3799

# Large first
SIZES="30 40 70 5 10 100"

# Memory configs per size (mem_mb:win_mb), 0:0 = AUTO
MEM_CONFIGS[5]="2048:64 4096:128 0:0"
MEM_CONFIGS[10]="4096:128 8192:256 0:0"
MEM_CONFIGS[30]="8192:256 16384:512 0:0"
MEM_CONFIGS[40]="8192:256 16384:512 0:0"
MEM_CONFIGS[70]="16384:512 24576:768 0:0"
MEM_CONFIGS[100]="16384:512 24576:768 32768:1024"

for GB in $SIZES; do
    nx=${NX[$GB]}; ny=${NY[$GB]}; nz=${NZ[$GB]}
    label="base_${GB}g"

    log "========== ${label} (${nx}x${ny}x${nz} ≈ ${GB}GB) =========="

    need_gb=$((GB * 3))
    if ! check_space $need_gb; then
        log "SKIP ${label}: not enough disk space"
        echo "${label},${GB},${nx},${ny},${nz},${GB},SKIP,0,0,0,0,0,0,0,0,0,0,0,0,disk_full" >> "$CSVFILE"
        continue
    fi

    RAWFILE="$BASEDIR/data_${GB}g.dat"
    LZ4FILE="$BASEDIR/data_${GB}g.erwt3d"
    RZFPFILE="$BASEDIR/data_${GB}g.rzfp"

    # Step 1: Generate raw data (16 threads)
    log "GEN ${GB}GB raw data ..."
    t0=$(date +%s%N)
    $GEN $nx $ny $nz "$RAWFILE" $THREADS 42 2>&1 | tee -a "$LOGFILE"
    t1=$(date +%s%N)
    gen_s=$(( (t1 - t0) / 1000000000 ))
    log "Raw generated in ${gen_s}s, size: $(du -h "$RAWFILE" | cut -f1)"

    # Step 2: LZ4 convert (max threads + max memory)
    log "CONVERT LZ4 ..."
    t0=$(date +%s%N)
    $CONVERT --input "$RAWFILE" --output "$LZ4FILE" \
        --nx $nx --ny $ny --nz $nz \
        --compress --raw-x-aux auto \
        --threads $THREADS --memory-limit-mb $CONVERT_MEM 2>&1 | tee -a "$LOGFILE"
    t1=$(date +%s%N)
    lz4_conv_s=$(( (t1 - t0) / 1000000000 ))
    log "LZ4 converted in ${lz4_conv_s}s, size: $(du -h "$LZ4FILE" | cut -f1)"

    # Step 3: LZ4 info
    $INFO "$LZ4FILE" 2>&1 | tee -a "$LOGFILE"

    # Step 4: LZ4 verify
    log "VERIFY LZ4 ..."
    $VERIFY --raw "$RAWFILE" --erwt3d "$LZ4FILE" \
        --nx $nx --ny $ny --nz $nz --samples 100000 2>&1 | tee -a "$LOGFILE" || true

    # Step 5: RZFP convert (max threads + max memory)
    log "CONVERT RZFP ..."
    t0=$(date +%s%N)
    $CONVERT_RZFP --input "$RAWFILE" --output "$RZFPFILE" \
        --nx $nx --ny $ny --nz $nz \
        --raw-x-aux auto --auto \
        --threads $THREADS 2>&1 | tee -a "$LOGFILE"
    t1=$(date +%s%N)
    rzfp_conv_s=$(( (t1 - t0) / 1000000000 ))
    log "RZFP converted in ${rzfp_conv_s}s, size: $(du -h "$RZFPFILE" | cut -f1)"

    # Step 6: RZFP verify
    log "VERIFY RZFP ..."
    $VERIFY_RZFP --raw "$RAWFILE" --rzfp "$RZFPFILE" \
        --nx $nx --ny $ny --nz $nz --samples 100000 2>&1 | tee -a "$LOGFILE" || true

    # Storage ratios
    raw_bytes=$(stat -c%s "$RAWFILE")
    lz4_bytes=$(stat -c%s "$LZ4FILE")
    rzfp_bytes=$(stat -c%s "$RZFPFILE")
    lz4_ratio=$(echo "scale=3; $lz4_bytes / $raw_bytes" | bc)
    rzfp_ratio=$(echo "scale=3; $rzfp_bytes / $raw_bytes" | bc)
    log "Storage: LZ4=${lz4_ratio}x, RZFP=${rzfp_ratio}x"

    # Step 7: LZ4 benchmarks
    for mcfg in ${MEM_CONFIGS[$GB]}; do
        mem_mb=${mcfg%%:*}
        win_mb=${mcfg##*:}

        if [ "$mem_mb" = "0" ]; then
            mem_flag=""
            win_flag=""
            cfg="AUTO"
        else
            mem_flag="--memory-limit-mb $mem_mb"
            win_flag="--hdd-read-window-bytes $((win_mb * 1024 * 1024)) --hdd-max-gap-bytes $((win_mb * 1024 * 1024 / 16))"
            cfg="M${mem_mb}"
        fi

        OUTDIR="$BASEDIR/out_${GB}g_lz4_${cfg}"
        mkdir -p "$OUTDIR"
        log "BENCH LZ4 ${cfg} ..."

        bench_out=$($BENCH --input "$LZ4FILE" --output-dir "$OUTDIR" \
            --threads $THREADS $mem_flag $win_flag --seed $SEED 2>&1)
        echo "$bench_out" | tee -a "$LOGFILE"

        T_c=$(echo "$bench_out" | grep -oP 'T_composite\s*=\s*\K[\d.]+' || echo "NA")
        T_xr=$(echo "$bench_out" | grep -oP 'T_xr\s*=\s*\K[\d.]+' || echo "NA")
        T_yr=$(echo "$bench_out" | grep -oP 'T_yr\s*=\s*\K[\d.]+' || echo "NA")
        T_zr=$(echo "$bench_out" | grep -oP 'T_zr\s*=\s*\K[\d.]+' || echo "NA")
        T_xc=$(echo "$bench_out" | grep -oP 'T_xc\s*=\s*\K[\d.]+' || echo "NA")
        T_yc=$(echo "$bench_out" | grep -oP 'T_yc\s*=\s*\K[\d.]+' || echo "NA")
        T_zc=$(echo "$bench_out" | grep -oP 'T_zc\s*=\s*\K[\d.]+' || echo "NA")
        RSS=$(echo "$bench_out" | grep -oP 'RSS\s*=\s*\K[\d.]+' || echo "NA")

        log "  LZ4 ${cfg}: T_composite=${T_c}s RSS=${RSS}GiB"
        echo "${label}_lz4_${cfg},${GB},${nx},${ny},${nz},${GB},lz4,${lz4_ratio},${mem_mb},${win_mb},${T_c},${T_xr},${T_yr},${T_zr},${T_xc},${T_yc},${T_zc},${RSS},${lz4_conv_s},0," >> "$CSVFILE"
        rm -rf "$OUTDIR"
    done

    # Step 8: RZFP benchmarks
    for mcfg in ${MEM_CONFIGS[$GB]}; do
        mem_mb=${mcfg%%:*}
        win_mb=${mcfg##*:}

        if [ "$mem_mb" = "0" ]; then
            mem_flag=""
            win_flag=""
            cfg="AUTO"
        else
            mem_flag="--memory-limit-mb $mem_mb"
            win_flag="--hdd-read-window-bytes $((win_mb * 1024 * 1024)) --hdd-max-gap-bytes $((win_mb * 1024 * 1024 / 16))"
            cfg="M${mem_mb}"
        fi

        OUTDIR="$BASEDIR/out_${GB}g_rzfp_${cfg}"
        mkdir -p "$OUTDIR"
        log "BENCH RZFP ${cfg} ..."

        bench_out=$($BENCH --input "$RZFPFILE" --output-dir "$OUTDIR" \
            --threads $THREADS $mem_flag $win_flag --seed $SEED 2>&1)
        echo "$bench_out" | tee -a "$LOGFILE"

        T_c=$(echo "$bench_out" | grep -oP 'T_composite\s*=\s*\K[\d.]+' || echo "NA")
        T_xr=$(echo "$bench_out" | grep -oP 'T_xr\s*=\s*\K[\d.]+' || echo "NA")
        T_yr=$(echo "$bench_out" | grep -oP 'T_yr\s*=\s*\K[\d.]+' || echo "NA")
        T_zr=$(echo "$bench_out" | grep -oP 'T_zr\s*=\s*\K[\d.]+' || echo "NA")
        T_xc=$(echo "$bench_out" | grep -oP 'T_xc\s*=\s*\K[\d.]+' || echo "NA")
        T_yc=$(echo "$bench_out" | grep -oP 'T_yc\s*=\s*\K[\d.]+' || echo "NA")
        T_zc=$(echo "$bench_out" | grep -oP 'T_zc\s*=\s*\K[\d.]+' || echo "NA")
        RSS=$(echo "$bench_out" | grep -oP 'RSS\s*=\s*\K[\d.]+' || echo "NA")

        log "  RZFP ${cfg}: T_composite=${T_c}s RSS=${RSS}GiB"
        echo "${label}_rzfp_${cfg},${GB},${nx},${ny},${nz},${GB},rzfp,${rzfp_ratio},${mem_mb},${win_mb},${T_c},${T_xr},${T_yr},${T_zr},${T_xc},${T_yc},${T_zc},${RSS},${rzfp_conv_s},0," >> "$CSVFILE"
        rm -rf "$OUTDIR"
    done

    # Cleanup
    log "Cleanup ${label} ..."
    rm -f "$RAWFILE" "$LZ4FILE" "$RZFPFILE"
    rm -f "${RZFPFILE}.xp" 2>/dev/null || true
    sync
    log "Done ${label}"
done

log "========== ALL TESTS COMPLETE =========="
log "Results: $CSVFILE"
cat "$CSVFILE" | tee -a "$LOGFILE"
