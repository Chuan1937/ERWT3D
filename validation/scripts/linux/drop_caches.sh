#!/usr/bin/env bash
# Linux-only cold OS page-cache boundary. It is not a claim of a cold SSD.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
sync
if [[ $EUID -eq 0 ]]; then
    echo 3 > /proc/sys/vm/drop_caches
elif sudo -n true 2>/dev/null; then
    echo 3 | sudo -n tee /proc/sys/vm/drop_caches >/dev/null
else
    echo "[ERROR] passwordless sudo (or root) is required to drop the OS page cache" >&2
    exit 1
fi
echo "[OK] cold OS page-cache boundary established"
