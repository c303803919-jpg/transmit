#!/bin/bash
# Build and run kvof_get_npu_ut in one command.
# Usage: bash run_kvof_get_npu_ut.sh [-v <SOC_VERSION>] [-b <BUILD_DIR>]
set -e

CURRENT_DIR=$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)
CODEX_DIR=$(cd "${CURRENT_DIR}/.." && pwd)
cd "${CODEX_DIR}"

SOC_VERSION="Ascend910B2"
BUILD_DIR="build_kvof_npu"

while [ $# -gt 0 ]; do
    case "$1" in
    -v | --soc-version)
        SOC_VERSION="$2"
        shift 2
        ;;
    -b | --build-dir)
        BUILD_DIR="$2"
        shift 2
        ;;
    *)
        echo "[ERROR] Unexpected option: $1"
        exit 1
        ;;
    esac
done

if [ -n "${ASCEND_INSTALL_PATH}" ]; then
    _ASCEND_INSTALL_PATH="${ASCEND_INSTALL_PATH}"
elif [ -n "${ASCEND_HOME_PATH}" ]; then
    _ASCEND_INSTALL_PATH="${ASCEND_HOME_PATH}"
elif [ -d "${HOME}/Ascend/ascend-toolkit/latest" ]; then
    _ASCEND_INSTALL_PATH="${HOME}/Ascend/ascend-toolkit/latest"
else
    _ASCEND_INSTALL_PATH="/usr/local/Ascend/ascend-toolkit/latest"
fi

if [ ! -f "${_ASCEND_INSTALL_PATH}/bin/setenv.bash" ]; then
    echo "[ERROR] setenv.bash not found: ${_ASCEND_INSTALL_PATH}/bin/setenv.bash"
    exit 1
fi

source "${_ASCEND_INSTALL_PATH}/bin/setenv.bash"

echo "[INFO] SOC_VERSION=${SOC_VERSION}"
echo "[INFO] ASCEND_CANN_PACKAGE_PATH=${_ASCEND_INSTALL_PATH}"
echo "[INFO] BUILD_DIR=${BUILD_DIR}"

rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"

cmake -B "${BUILD_DIR}" \
    -DSOC_VERSION="${SOC_VERSION}" \
    -DASCEND_CANN_PACKAGE_PATH="${_ASCEND_INSTALL_PATH}"

cmake --build "${BUILD_DIR}" -j --target kvof_get_npu_ut

"./${BUILD_DIR}/kvof_get_npu_ut" | tee "${BUILD_DIR}/kvof_get_npu_ut_output.txt"

grep -q "kvof_get_npu_ut passed" "${BUILD_DIR}/kvof_get_npu_ut_output.txt"

echo "[INFO] kvof_get_npu_ut completed successfully"
