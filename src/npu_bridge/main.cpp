/**
 * @file main.cpp
 *
 * NPU build entry point. Allocates a single shared GM region, stages the
 * KeyDevice, launches the NPU kernel and concurrently runs the host
 * three-round service from the main thread.
 *
 * Reference flow:
 *   1) host: aclrtMalloc() shared GM = sizeof(ControlBlock) + kHbmAreaBytes
 *   2) host: fill ControlBlock::key from a kv_transfer::Key, flag=kFlagIdle
 *   3) host: launch kv_request_kernel<<<...>>>(ctrl, hbm)
 *   4) host: service_one_request() runs concurrently with the kernel:
 *      wait kFlagRequest -> 3 metadata rounds -> kFlagResponse -> wait kFlagDone
 *   5) host: aclrtSynchronizeStream(), free, exit.
 *
 * The HBM result area lives in the same allocation, kHbmAreaBytes after the
 * ControlBlock. The kernel is given the base pointer of the HBM area so the
 * gather output is laid out as one contiguous run of tokens.
 */

#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>

#include "acl/acl.h"

#include "../query/kv_transfer.h"
#include "npu_kv_host.h"
#include "npu_kv_request.h"

extern void kv_request_kernel_do(uint32_t blockDim,
                                 void    *stream,
                                 uint8_t *control,
                                 uint8_t *hbm_buf);

#define CHECK_ACL(x)                                              \
    do {                                                          \
        aclError __ret = (x);                                     \
        if (__ret != ACL_SUCCESS) {                               \
            std::cerr << #x << " failed, ret=" << __ret << "\n";  \
            return 1;                                             \
        }                                                         \
    } while (0)

int32_t main(int /*argc*/, char ** /*argv*/) {
    CHECK_ACL(aclInit(nullptr));
    int32_t deviceId = 0;
    CHECK_ACL(aclrtSetDevice(deviceId));
    aclrtStream stream = nullptr;
    CHECK_ACL(aclrtCreateStream(&stream));

    // ------------------------------------------------------------------
    // Build the request key on the host. (Same shape as kv_transfer::Key.)
    // ------------------------------------------------------------------
    kv_transfer::Key key;
    key.reqid    = "req-npu-001";
    key.layerid  = 7;
    key.tokenids = {0, 1, 2, 3, 4, 5, 8, 17, 19, 33};

    // ------------------------------------------------------------------
    // Allocate a single contiguous GM region for ControlBlock + HBM area.
    // The kernel and the host both read/write this region; the volatile
    // flag inside ControlBlock provides the hand-shake.
    // ------------------------------------------------------------------
    void *gm = nullptr;
    CHECK_ACL(aclrtMalloc(&gm, npu_kv::kSharedGmBytes,
                          ACL_MEM_MALLOC_HUGE_FIRST));

    // Use a host shadow to populate the control block, then memcpy in.
    npu_kv::ControlBlock host_ctrl{};
    if (!npu_kv::fill_key_device(host_ctrl.key, key)) {
        std::cerr << "fill_key_device failed (key too large)\n";
        aclrtFree(gm);
        return 1;
    }
    host_ctrl.flag       = npu_kv::kFlagIdle;
    host_ctrl.token_size = static_cast<std::uint32_t>(kv_transfer::kTokenSize);

    CHECK_ACL(aclrtMemcpy(gm, npu_kv::kSharedGmBytes,
                          &host_ctrl, sizeof(host_ctrl),
                          ACL_MEMCPY_HOST_TO_DEVICE));

    auto *ctrl = reinterpret_cast<npu_kv::ControlBlock *>(gm);
    auto *hbm  = reinterpret_cast<std::uint8_t *>(gm) + sizeof(npu_kv::ControlBlock);

    // ------------------------------------------------------------------
    // Launch the kernel. The host service runs concurrently from main().
    // ------------------------------------------------------------------
    constexpr uint32_t blockDim = 1;
    kv_request_kernel_do(blockDim, stream,
                         reinterpret_cast<std::uint8_t *>(ctrl),
                         hbm);

    // The host services the request synchronously. service_one_request()
    // will: wait kFlagRequest -> three rounds -> kFlagResponse -> wait kFlagDone.
    const bool ok = npu_kv::service_one_request(
        ctrl, hbm, npu_kv::kHbmAreaBytes,
        std::chrono::milliseconds(10000));

    CHECK_ACL(aclrtSynchronizeStream(stream));

    if (!ok) {
        std::cerr << "[Host] hand-shake failed (status=" << ctrl->status << ")\n";
    } else {
        std::cout << "[Host] hand-shake completed: hits=" << ctrl->num_hits
                  << " misses=" << ctrl->num_miss_tokens
                  << " total=" << ctrl->total_tokens << "\n";
    }

    CHECK_ACL(aclrtFree(gm));
    CHECK_ACL(aclrtDestroyStream(stream));
    CHECK_ACL(aclrtResetDevice(deviceId));
    CHECK_ACL(aclFinalize());
    return ok ? 0 : 1;
}
