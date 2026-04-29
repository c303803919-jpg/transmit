// npu_kv_sim.cpp -- CPU-side simulator for the NPU kernel.
//
// Behaviour is intentionally identical to npu_kv_kernel.cpp so the protocol
// can be unit-tested without an NPU.

#include "npu_kv_sim.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace npu_kv {

SimResult simulate_npu_kernel(ControlBlock*             ctrl,
                              const void*               hbm_buf,
                              std::chrono::milliseconds timeout) {
    using clock = std::chrono::steady_clock;
    SimResult res;

    if (ctrl == nullptr || hbm_buf == nullptr) {
        return res;
    }

    // Step 1: NPU -> CPU. Raise the request flag. release-fence so the host
    // sees a fully-published flag value once it observes kFlagRequest.
    std::atomic_thread_fence(std::memory_order_release);
    ctrl->flag = kFlagRequest;

    // Step 2: spin for response / error / timeout.
    const auto deadline = clock::now() + timeout;
    while (true) {
        std::atomic_thread_fence(std::memory_order_acquire);
        std::uint32_t f = ctrl->flag;
        if (f == kFlagResponse || f == kFlagError) {
            res.observed_response = true;
            res.status_seen       = ctrl->status;
            break;
        }
        if (clock::now() >= deadline) {
            return res;  // not finished
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    // Step 3: consume payload (only on success).
    if (ctrl->flag == kFlagResponse && ctrl->status == kStatusOk) {
        res.total_tokens_seen = ctrl->total_tokens;
        res.token_size_seen   = ctrl->token_size;
        res.first_bytes.reserve(ctrl->total_tokens);
        const unsigned char* hbm =
            static_cast<const unsigned char*>(hbm_buf);
        for (std::uint32_t i = 0; i < ctrl->total_tokens; ++i) {
            res.first_bytes.push_back(
                hbm[static_cast<std::size_t>(i) * ctrl->token_size]);
        }
    }

    // Step 4: NPU -> CPU. Done.
    std::atomic_thread_fence(std::memory_order_release);
    ctrl->flag      = kFlagDone;
    res.finished    = true;
    return res;
}

}  // namespace npu_kv
