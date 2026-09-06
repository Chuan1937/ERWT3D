#!/usr/bin/env bash
# Drop OS page cache (requires root/sudo)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/utils.sh"

if [[ $EUID -ne 0 ]] && ! sudo -n true 2>/dev/null; then
    echo "[ERROR] Need root or passwordless sudo to drop caches"
    echo "        Run: sudo visudo  # add: $(whoami) ALL=(ALL) NOPASSWD: /usr/bin/tee /proc/sys/vm/drop_caches"
    exit 1
fi

echo "Dropping OS page cache..."
sync
if [[ $EUID -eq 0 ]]; then
    echo 3 > /proc/sys/vm/drop_caches
else
    echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
fi
echo "[OK] Page cache dropped at $(date -Iseconds)"

# Verify
echo "Memory state after drop:"
free -h
