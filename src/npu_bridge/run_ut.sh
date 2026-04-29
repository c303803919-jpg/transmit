#!/bin/bash
# Build and run the host-only unit tests for the NPU<->CPU KV bridge.
# Does not require the CANN tool-chain or an NPU.
set -e
CURRENT_DIR=$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)
cd "$CURRENT_DIR"

make clean
make -j
./npu_kv_ut
