#!/usr/bin/env bash
# Shared Linux-only formal benchmark helpers. No storage paths are guessed.
set -euo pipefail

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "[ERROR] Linux formal benchmark scripts must run on Linux." >&2
    exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VALIDATION_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
PROJECT_ROOT="$(cd "$VALIDATION_DIR/.." && pwd)"
BUILD_DIR="${ERWT3D_BUILD_DIR:-$PROJECT_ROOT/build}"
RESULT_ROOT="${ERWT3D_RESULT_ROOT:-$VALIDATION_DIR}"

require_roots() {
    local name
    for name in ERWT3D_DATASET_ROOT ERWT3D_SSD_ROOT ERWT3D_HDD_ROOT ERWT3D_RESULT_ROOT; do
        if [[ -z "${!name:-}" ]]; then
            echo "[ERROR] $name is required; export it or load validation/configs/linux.json." >&2
            return 1
        fi
    done
}

require_binary() {
    local binary="$1"
    [[ -x "$BUILD_DIR/$binary" ]] || { echo "[ERROR] Missing binary: $BUILD_DIR/$binary" >&2; return 1; }
}

sha256_file() { sha256sum "$1" | awk '{print $1}'; }

write_failed_json() {
    local path="$1" exit_code="$2" message="$3"
    python3 - "$path" "$exit_code" "$message" <<'PY'
import json, pathlib, sys
pathlib.Path(sys.argv[1]).parent.mkdir(parents=True, exist_ok=True)
pathlib.Path(sys.argv[1]).write_text(json.dumps({"status":"FAILED", "exit_code":int(sys.argv[2]), "error":sys.argv[3]}, indent=2) + "\n")
PY
}

copy_and_verify() {
    local source="$1" destination="$2"
    mkdir -p "$(dirname "$destination")"
    cp --reflink=auto -- "$source" "$destination"
    local source_sum destination_sum
    source_sum="$(sha256_file "$source")"; destination_sum="$(sha256_file "$destination")"
    [[ "$source_sum" == "$destination_sum" ]] || { echo "[ERROR] SHA256 mismatch after copy: $destination" >&2; return 1; }
}
