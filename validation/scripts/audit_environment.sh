#!/usr/bin/env bash
# Audit server environment and write JSON to validation/environment/
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/utils.sh"

OUT_DIR="$VALIDATION_DIR/environment"
mkdir -p "$OUT_DIR"

echo "=== ERWT3D Environment Audit ==="

# --- CPU ---
CPU_MODEL=$(lscpu | grep "Model name" | sed 's/.*:\s*//')
PHYSICAL_CORES=$(lscpu | grep "Core(s) per socket" | awk '{print $NF}')
SOCKETS=$(lscpu | grep "Socket(s)" | awk '{print $NF}')
LOGICAL_CORES=$(nproc)
NUMA_NODES=$(lscpu | grep "NUMA node(s)" | awk '{print $NF}')
CPU_MHZ=$(lscpu | grep "CPU MHz" | awk '{print $NF}')
CPU_FLAGS=$(lscpu | grep "Flags" | head -1)

# --- Memory ---
TOTAL_RAM=$(free -h | awk '/Mem:/ {print $2}')
AVAIL_RAM=$(free -h | awk '/Mem:/ {print $7}')

# --- OS ---
OS_INFO=$(uname -a)
DISTRO=$(cat /etc/os-release 2>/dev/null | grep PRETTY_NAME | cut -d= -f2 | tr -d '"')
KERNEL=$(uname -r)

# --- Compiler ---
CC_VERSION=$(${CC:-gcc} --version 2>/dev/null | head -1 || echo "unknown")
CXX_VERSION=$(${CXX:-g++} --version 2>/dev/null | head -1 || echo "unknown")
CMAKE_VERSION=$(cmake --version 2>/dev/null | head -1 || echo "unknown")

# --- Disk ---
echo ""
echo "Block devices:"
lsblk -o NAME,MODEL,SIZE,ROTA,TYPE,FSTYPE,MOUNTPOINT 2>/dev/null || lsblk

echo ""
echo "Disk space:"
df -h | grep -E '^/dev/'

# --- Write server.json ---
cat > "$OUT_DIR/server.json" <<JSONEOF
{
  "audit_timestamp": "$(date -Iseconds)",
  "hostname": "$(hostname)",
  "cpu": {
    "model": "$CPU_MODEL",
    "physical_cores": $PHYSICAL_CORES,
    "sockets": $SOCKETS,
    "logical_cores": $LOGICAL_CORES,
    "numa_nodes": $NUMA_NODES,
    "frequency_mhz": "$CPU_MHZ",
    "flags": "$CPU_FLAGS"
  },
  "memory": {
    "total": "$TOTAL_RAM",
    "available": "$AVAIL_RAM"
  },
  "os": {
    "info": "$OS_INFO",
    "distribution": "$DISTRO",
    "kernel": "$KERNEL"
  },
  "compiler": {
    "cc": "$CC_VERSION",
    "cxx": "$CXX_VERSION",
    "cmake": "$CMAKE_VERSION"
  }
}
JSONEOF

echo ""
echo "[OK] Written: $OUT_DIR/server.json"
cat "$OUT_DIR/server.json"

echo ""
echo "=== Environment audit complete ==="
