#!/usr/bin/env bash
# Common utilities for ERWT3D paper benchmark scripts

set -euo pipefail

VALIDATION_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_ROOT="$(cd "$VALIDATION_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

# --- Git info ---
get_git_commit() {
    git -C "$PROJECT_ROOT" rev-parse HEAD
}

get_git_status() {
    git -C "$PROJECT_ROOT" status --porcelain
}

is_git_dirty() {
    [[ -n "$(get_git_status)" ]]
}

# --- System state ---
record_system_state() {
    echo "=== System State ==="
    echo "Timestamp: $(date -Iseconds)"
    echo "Hostname: $(hostname)"
    echo "Uptime: $(uptime)"
    echo ""
    free -h
    echo ""
    echo "Top CPU processes:"
    ps aux --sort=-%cpu | head -6
    echo "===================="
}

# --- Cache management ---
drop_caches() {
    if [[ $EUID -eq 0 ]]; then
        sync
        echo 3 > /proc/sys/vm/drop_caches
        echo "[OK] Page cache dropped"
    else
        echo "[WARN] Not root, attempting sudo..."
        sync
        echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
        echo "[OK] Page cache dropped via sudo"
    fi
}

# --- Run ID ---
generate_run_id() {
    local device="$1"
    local method="$2"
    local dataset="$3"
    local size="$4"
    local axis="$5"
    local pattern="$6"
    local run_num="$7"
    echo "$(date +%Y%m%d)_${device}_${method}_${dataset}_${size}_${axis}_${pattern}_run$(printf '%02d' "$run_num")"
}

# --- Result paths ---
result_json_path() {
    local run_id="$1"
    local category="$2"
    echo "$VALIDATION_DIR/raw_results/${category}/${run_id}.json"
}

result_csv_path() {
    local run_id="$1"
    local category="$2"
    echo "$VALIDATION_DIR/raw_results/${category}/${run_id}.csv"
}

log_stdout_path() {
    local run_id="$1"
    echo "$VALIDATION_DIR/logs/${run_id}.stdout.log"
}

log_stderr_path() {
    local run_id="$1"
    echo "$VALIDATION_DIR/logs/${run_id}.stderr.log"
}

# --- Validation ---
check_binary() {
    local bin="$1"
    if [[ ! -x "$BUILD_DIR/$bin" ]]; then
        echo "[ERROR] Binary not found: $BUILD_DIR/$bin"
        echo "        Build first: cmake --build $BUILD_DIR -j"
        return 1
    fi
}

check_file() {
    local f="$1"
    if [[ ! -f "$f" ]]; then
        echo "[ERROR] File not found: $f"
        return 1
    fi
}

# --- Timing ---
time_to_seconds() {
    # Convert bash TIMEFORMAT output to seconds
    local t="$1"
    echo "$t"
}

# --- SHA256 ---
compute_sha256() {
    local f="$1"
    sha256sum "$f" | awk '{print $1}'
}

echo "[utils.sh] Loaded from $VALIDATION_DIR"
