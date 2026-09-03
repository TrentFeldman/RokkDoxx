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
  src/svc/pattern_io.cpp
)
LINK=()
GPU_TEST=""
if [[ "${ROKK_OPENCL:-0}" == "1" ]]; then
  # Embed the kernel text (same as CMake's configure_file step).
  mkdir -p build/generated
  {
    printf '#pragma once\nnamespace rokkdoxx::svc {\n'
    printf 'inline constexpr const char* kBedrockCoreSrc = R"ROKKCL(\n'
    cat src/gen/bedrock_core.h
    printf ')ROKKCL";\ninline constexpr const char* kSearchTileSrc = R"ROKKCL(\n'
    cat src/cl/search_tile.cl
    printf ')ROKKCL";\n}\n'
  } > build/generated/kernel_sources.h

  FLAGS+=(-DROKK_ENABLE_OPENCL -DCL_TARGET_OPENCL_VERSION=120 -Ibuild/generated)
  SVC+=(src/svc/opencl_worker.cpp)
  LINK+=(-lOpenCL)
  GPU_TEST="build/test_gpu"
fi

CORE=(src/gen/bedrock.cpp "${SVC[@]}")

"$CXX" "${FLAGS[@]}" tools/dump_bedrock.cpp src/gen/bedrock.cpp -o build/dump_bedrock
"$CXX" "${FLAGS[@]}" tools/rokktui.cpp    "${CORE[@]}" "${LINK[@]}" -o build/rokktui
"$CXX" "${FLAGS[@]}" tools/rokksearch.cpp "${CORE[@]}" "${LINK[@]}" -o build/rokksearch
"$CXX" "${FLAGS[@]}" tests/test_bedrock.cpp src/gen/bedrock.cpp -o build/test_bedrock
"$CXX" "${FLAGS[@]}" tests/test_search.cpp "${CORE[@]}" "${LINK[@]}" -o build/test_search
if [[ -n "$GPU_TEST" ]]; then
  "$CXX" "${FLAGS[@]}" tests/test_gpu.cpp "${CORE[@]}" "${LINK[@]}" -o build/test_gpu
fi

echo "built: build/{dump_bedrock,rokktui,rokksearch,test_*}"

if [[ "${1:-}" == "test" ]]; then
  ./build/test_bedrock
  ./build/test_search
  [[ -n "$GPU_TEST" ]] && ./build/test_gpu
  python3 tests/diff_test.py ./build/dump_bedrock
fi
