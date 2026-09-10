#!/usr/bin/env bash
# Legacy utility shim. New formal scripts source scripts/linux/common.sh instead.
set -euo pipefail
VALIDATION_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_ROOT="$(cd "$VALIDATION_DIR/.." && pwd)"
BUILD_DIR="${ERWT3D_BUILD_DIR:-$PROJECT_ROOT/build}"
get_git_commit() { git -C "$PROJECT_ROOT" rev-parse HEAD; }
compute_sha256() { sha256sum "$1" | awk '{print $1}'; }
check_binary() { [[ -x "$BUILD_DIR/$1" ]] || { echo "[ERROR] Missing binary $BUILD_DIR/$1" >&2; return 1; }; }
check_file() { [[ -f "$1" ]] || { echo "[ERROR] Missing file $1" >&2; return 1; }; }
