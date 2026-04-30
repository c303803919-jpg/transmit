#!/usr/bin/env bash
# Configure / build / run the mem_pool UT (test_mem_pool)
# from the top-level KVoF_PR CMake project.

set -euo pipefail

BUILD_DIR="build"
CXX_COMPILER=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -b|--build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        -c|--cxx)
            CXX_COMPILER="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: bash run_mem_pool_ut.sh [-b <build_dir>] [-c <cxx_compiler>]"
            exit 0
            ;;
        *)
            echo "Unknown argument: $1"
            echo "Usage: bash run_mem_pool_ut.sh [-b <build_dir>] [-c <cxx_compiler>]"
            exit 1
            ;;
    esac
done

# Script lives in KVoF_PR/01_ut/, project root is one level up.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_PATH="$PROJECT_DIR/$BUILD_DIR"

CMAKE_ARGS=("-S" "$PROJECT_DIR" "-B" "$BUILD_PATH")
if [[ -n "$CXX_COMPILER" ]]; then
    CMAKE_ARGS+=("-DCMAKE_CXX_COMPILER=$CXX_COMPILER")
fi

echo "[INFO] Configuring KVoF_PR in $BUILD_PATH"
cmake "${CMAKE_ARGS[@]}"

echo "[INFO] Building test_mem_pool"
cmake --build "$BUILD_PATH" -j --target test_mem_pool

echo "[INFO] Running test_mem_pool"
"$BUILD_PATH/test_mem_pool"

echo "[INFO] mem_pool test_mem_pool completed successfully"
