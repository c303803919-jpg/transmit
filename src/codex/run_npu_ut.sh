#!/bin/bash
# Build and run the Kvof NPU-specific UT.
# Usage: bash run_npu_ut.sh [-v <SOC_VERSION>]
set -e

CURRENT_DIR=$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)
cd "$CURRENT_DIR"

SOC_VERSION="Ascend910B2"

while [ $# -gt 0 ]; do
    case "$1" in
    -v | --soc-version)
        SOC_VERSION="$2"
        shift 2
        ;;
    *) echo "[ERROR]: Unexpected option: $1"; exit 1;;
    esac
done

if [ -n "$ASCEND_INSTALL_PATH" ]; then
    _ASCEND_INSTALL_PATH=$ASCEND_INSTALL_PATH
elif [ -n "$ASCEND_HOME_PATH" ]; then
    _ASCEND_INSTALL_PATH=$ASCEND_HOME_PATH
elif [ -d "$HOME/Ascend/ascend-toolkit/latest" ]; then
    _ASCEND_INSTALL_PATH=$HOME/Ascend/ascend-toolkit/latest
else
    _ASCEND_INSTALL_PATH=/usr/local/Ascend/ascend-toolkit/latest
fi

source "$_ASCEND_INSTALL_PATH/bin/setenv.bash"
echo "[INFO]: Current compile soc version is ${SOC_VERSION}"

rm -rf build_npu
mkdir -p build_npu
cmake -B build_npu \
    -DSOC_VERSION=${SOC_VERSION} \
    -DASCEND_CANN_PACKAGE_PATH=${_ASCEND_INSTALL_PATH}
cmake --build build_npu -j --target npu_token_get_index_ut

./build_npu/npu_token_get_index_ut | tee npu_ut_output.txt

grep -q "\[CPU\] calling Kvof::token_get_index on CPU host" npu_ut_output.txt
grep -q "\[NPU\] issuing vector-key request on NPU" npu_ut_output.txt
grep -q "\[CPU\] NPU observed first byte per token" npu_ut_output.txt
grep -q "\[CPU\] npu_token_get_index_ut passed" npu_ut_output.txt
