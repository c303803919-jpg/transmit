// npu_kv_sim.h -- pure-CPU simulation of the NPU kernel.
//
// The real Ascend C kernel lives in npu_kv_kernel.cpp and requires the CANN
// toolchain. For host-only unit tests we mirror the same hand-shake here in
// plain C++ so the protocol can be exercised without an NPU. The function
// runs against the same shared GM layout (ControlBlock + HBM area), so the
// host service code is exactly the code path used in the real build.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "npu_kv_request.h"

namespace npu_kv {

// Output of a single simulated kernel run, for verification in tests.
struct SimResult {
    bool          finished {false};       // kernel reached kFlagDone
    bool          observed_response {false};
    std::uint32_t status_seen {0};        // ctrl->status at response time
    std::uint32_t total_tokens_seen {0};
    std::uint32_t token_size_seen {0};
    std::vector<unsigned char> first_bytes;   // first byte of each token read
};

// Mirror of npu_kv_kernel.cpp's behaviour:
//   * raise kFlagRequest,
//   * spin for kFlagResponse / kFlagError,
//   * read the first byte of each returned token,
//   * raise kFlagDone.
//
// Returns once the simulated kernel exits or `timeout` fires.
SimResult simulate_npu_kernel(ControlBlock*             ctrl,
                              const void*               hbm_buf,
                              std::chrono::milliseconds timeout =
                                  std::chrono::milliseconds(5000));

}  // namespace npu_kv
