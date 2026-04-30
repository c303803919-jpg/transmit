#!/usr/bin/env bash

set -euo pipefail

BUILD_DIR="build_kvof_cpu"
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

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

CMAKE_ARGS=("-S" "$SCRIPT_DIR" "-B" "$SCRIPT_DIR/$BUILD_DIR")
if [[ -n "$CXX_COMPILER" ]]; then
    CMAKE_ARGS+=("-DCMAKE_CXX_COMPILER=$CXX_COMPILER")
fi

echo "[INFO] Configuring kvof_cpu UT in $SCRIPT_DIR/$BUILD_DIR"
cmake "${CMAKE_ARGS[@]}"

echo "[INFO] Building token_get_index_ut"
cmake --build "$SCRIPT_DIR/$BUILD_DIR" -j --target token_get_index_ut

echo "[INFO] Running token_get_index_ut"
"$SCRIPT_DIR/$BUILD_DIR/token_get_index_ut"

echo "[INFO] kvof_cpu token_get_index_ut completed successfully"
