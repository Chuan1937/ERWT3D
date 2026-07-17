#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-p4}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}"

rm -rf "${BUILD_DIR}"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DERWT3D_ENABLE_RZFP=ON \
  -DERWT3D_NATIVE_OPT=OFF

cmake --build "${BUILD_DIR}" -j"${JOBS}"
ctest --test-dir "${BUILD_DIR}" --output-on-failure -j8

echo
echo "Clean build and CTest completed."

if [[ -z "${ERWT3D_RZFP_INPUT:-}" ]]; then
  echo "Set ERWT3D_RZFP_INPUT and ERWT3D_OUTPUT_ROOT to run HDD benchmarks."
  exit 0
fi

OUTPUT_ROOT="${ERWT3D_OUTPUT_ROOT:-${ROOT_DIR}/p4-results}"
ROUNDS="${ERWT3D_ROUNDS:-3}"
MEMORY_LIMIT="${ERWT3D_MEMORY_LIMIT_MB:-auto}"

mkdir -p "${OUTPUT_ROOT}"

run_mode() {
  local mode="$1"
  local output="${OUTPUT_ROOT}/${mode}"
  mkdir -p "${output}"

  "${BUILD_DIR}/erwt3d_bench_rzfp" \
    --input "${ERWT3D_RZFP_INPUT}" \
    --output-dir "${output}" \
    --hdd \
    --read-strategy auto \
    --memory-limit-mb "${MEMORY_LIMIT}" \
    --window-cache-mb auto \
    --benchmark-cache-mode "${mode}" \
    --group-order official \
    --rounds "${ROUNDS}" \
    --seed 20260511
}

run_mode cold-group
run_mode cold-round
run_mode stable-auto
run_mode warm

echo
echo "P4 HDD matrix completed under ${OUTPUT_ROOT}."
