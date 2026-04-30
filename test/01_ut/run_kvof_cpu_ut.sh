#!/usr/bin/env bash
# Configure / build / run the kvof_cpu UT (token_get_index_ut)
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
            echo "Usage: bash run_kvof_cpu_ut.sh [-b <build_dir>] [-c <cxx_compiler>]"
            exit 0
            ;;
        *)
            echo "Unknown argument: $1"
            echo "Usage: bash run_kvof_cpu_ut.sh [-b <build_dir>] [-c <cxx_compiler>]"
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

echo "[INFO] Building token_get_index_ut"
cmake --build "$BUILD_PATH" -j --target token_get_index_ut

echo "[INFO] Running token_get_index_ut"
"$BUILD_PATH/01_ut/token_get_index_ut"

echo "[INFO] kvof_cpu token_get_index_ut completed successfully"
