#!/usr/bin/env bash
# Compatibility entrypoint. Formal experiments moved to scripts/linux/run_all.sh.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$SCRIPT_DIR/linux/run_all.sh" "$@"
