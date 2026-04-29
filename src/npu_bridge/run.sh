#!/bin/bash
# Build and run the full NPU bridge sample.
#   Usage: bash run.sh [-v <SOC_VERSION>]
#
# Mirrors samples-master/operator/ascendc/0_introduction/0_helloworld/run.sh.
set -e
CURRENT_DIR=$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)
cd "$CURRENT_DIR"

SHORT=v:
LONG=soc-version:
OPTS=$(getopt -a --options $SHORT --longoptions $LONG -- "$@")
eval set -- "$OPTS"
SOC_VERSION="Ascend910B2"

while :; do
    case "$1" in
    -v | --soc-version) SOC_VERSION="$2"; shift 2;;
    --) shift; break;;
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

rm -rf build
mkdir -p build
cmake -B build \
    -DSOC_VERSION=${SOC_VERSION} \
    -DASCEND_CANN_PACKAGE_PATH=${_ASCEND_INSTALL_PATH}
cmake --build build -j

./build/npu_kv_main
