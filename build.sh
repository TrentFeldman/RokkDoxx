#!/usr/bin/env bash
# Fallback build for environments without make/ninja (CMake is the primary path:
# `cmake -B build && cmake --build build && ctest --test-dir build`).
#
# OpenCL: set ROKK_OPENCL=1 to compile the GPU worker (needs opencl-headers +
# libOpenCL). Off by default so this works on a bare box.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build

CXX="${CXX:-g++}"
FLAGS=(-std=c++20 -O2 -Wall -Wextra -Isrc -pthread)

SVC=(
  src/svc/search_service.cpp
  src/svc/workers.cpp
  src/svc/client.cpp
  src/svc/protocol.cpp
  src/svc/daemon.cpp
  src/svc/pattern_io.cpp
)
LINK=()
GPU_TEST=""
if [[ "${ROKK_OPENCL:-0}" == "1" ]]; then
  FLAGS+=(-DROKK_ENABLE_OPENCL -DROKK_SRC_DIR="\"$PWD/src\"")
  SVC+=(src/svc/opencl_worker.cpp)
  LINK+=(-lOpenCL)
  GPU_TEST="build/test_gpu"
fi

CORE=(src/gen/bedrock.cpp "${SVC[@]}")

"$CXX" "${FLAGS[@]}" tools/dump_bedrock.cpp src/gen/bedrock.cpp -o build/dump_bedrock
"$CXX" "${FLAGS[@]}" tools/rokktui.cpp    "${CORE[@]}" "${LINK[@]}" -o build/rokktui
"$CXX" "${FLAGS[@]}" tools/rokksearch.cpp "${CORE[@]}" "${LINK[@]}" -o build/rokksearch
"$CXX" "${FLAGS[@]}" tools/rokkd.cpp      "${CORE[@]}" "${LINK[@]}" -o build/rokkd
"$CXX" "${FLAGS[@]}" tests/test_bedrock.cpp src/gen/bedrock.cpp -o build/test_bedrock
"$CXX" "${FLAGS[@]}" tests/test_search.cpp "${CORE[@]}" "${LINK[@]}" -o build/test_search
"$CXX" "${FLAGS[@]}" tests/test_daemon.cpp "${CORE[@]}" "${LINK[@]}" -o build/test_daemon
if [[ -n "$GPU_TEST" ]]; then
  "$CXX" "${FLAGS[@]}" tests/test_gpu.cpp "${CORE[@]}" "${LINK[@]}" -o build/test_gpu
fi

echo "built: build/{dump_bedrock,rokktui,rokksearch,rokkd,test_*}"

if [[ "${1:-}" == "test" ]]; then
  ./build/test_bedrock
  ./build/test_search
  ./build/test_daemon
  [[ -n "$GPU_TEST" ]] && ./build/test_gpu
  python3 tests/diff_test.py ./build/dump_bedrock
fi
