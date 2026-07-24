#!/bin/bash
# prepare_wsl_ssd.sh — Setup WSL native SSD environment for ERWT3D SSD benchmarks

set -euo pipefail

SSD_ROOT="${HOME}/erwt3d_ssd"
G_DRIVE="/mnt/g/cup"

echo "=== ERWT3D SSD Environment Setup ==="
echo "Target directory: ${SSD_ROOT}"
echo "Source (G: drive): ${G_DRIVE}"

mkdir -p "${SSD_ROOT}"/{data,formats,results,positions,benchmark,logs,tmp}

echo
echo "Directory structure created."
echo
echo "To copy raw data (67.2 GiB total), run:"
echo "  rsync -ah --info=progress2 ${G_DRIVE}/cup_3d_small.dat ${SSD_ROOT}/data/"
echo "  rsync -ah --info=progress2 ${G_DRIVE}/cup_3d_big.dat   ${SSD_ROOT}/data/"
echo "  sync"
echo
echo "Verify sizes:"
echo "  cup_3d_small.dat : 19,271,755,620 bytes"
echo "  cup_3d_big.dat   : 52,850,412,000 bytes"
echo
echo "After copying, run SHA256 verification:"
echo "  sha256sum ${G_DRIVE}/cup_3d_small.dat ${SSD_ROOT}/data/cup_3d_small.dat"
echo "  sha256sum ${G_DRIVE}/cup_3d_big.dat   ${SSD_ROOT}/data/cup_3d_big.dat"
