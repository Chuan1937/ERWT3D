#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

PREFIX="${PREFIX:-/home/chuan/code/ERWT3D/deps/zfp}"
LZ4_INC="${LZ4_INCLUDE_DIR:-/home/chuan/code/ERWT3D/deps/lz4/include}"
LZ4_LIB="${LZ4_LIBRARY:-/usr/lib64/liblz4.so.1}"

echo "=== 1. Release build with RZFP ==="
rm -rf build-release
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
      -DERWT3D_ENABLE_RZFP=ON \
      -DERWT3D_NATIVE_OPT=ON \
      -DCMAKE_PREFIX_PATH="$PREFIX" \
      -DLZ4_INCLUDE_DIR="$LZ4_INC" \
      -DLZ4_LIBRARY="$LZ4_LIB"
cmake --build build-release -j$(nproc)
echo "PASS: Release build"

echo "=== 2. CTest (Release) ==="
ctest --test-dir build-release --output-on-failure --timeout 180 -j$(nproc)
echo "PASS: CTest"

echo "=== 3. No-RZFP build ==="
rm -rf build-no-rzfp
cmake -S . -B build-no-rzfp -DCMAKE_BUILD_TYPE=Release \
      -DERWT3D_ENABLE_RZFP=OFF \
      -DERWT3D_NATIVE_OPT=ON \
      -DLZ4_INCLUDE_DIR="$LZ4_INC" \
      -DLZ4_LIBRARY="$LZ4_LIB"
cmake --build build-no-rzfp -j$(nproc)
echo "PASS: No-RZFP build"

echo ""
echo "=== ALL VALIDATION PASSED ==="
