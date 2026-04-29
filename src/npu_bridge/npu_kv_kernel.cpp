/**
 * @file npu_kv_kernel.cpp
 *
 * NPU side of the CPU<->NPU KV-fetch hand-shake.
 *
 * The kernel:
 *   1) reads the KeyDevice that the host pre-staged in GM,
 *   2) flips ControlBlock::flag to kFlagRequest to wake the host,
 *   3) busy-waits until the host writes kFlagResponse,
 *   4) "consumes" the gathered data (here we just sample-read every token
 *      so the read path is exercised; a real kernel would copy into UB and
 *      run compute), and
 *   5) flips the flag to kFlagDone so the host can synchronise the stream.
 *
 * Build: this file is only compiled in NPU/sim builds (see CMakeLists.txt).
 *        The CPU UT uses npu_kv_sim.cpp, which mirrors the same protocol in
 *        plain C++ so the protocol can be exercised on a host machine.
 */

#include "kernel_operator.h"

#include "npu_kv_request.h"

extern "C" __global__ __aicore__ void kv_request_kernel(GM_ADDR control,
                                                        GM_ADDR hbm_buf)
{
    // Single-core only -- only block 0 drives the hand-shake. Other blocks
    // exit immediately so the launch can still use a >1 grid for warm-up.
    if (AscendC::GetBlockIdx() != 0) {
        return;
    }

    auto *ctrl = reinterpret_cast<__gm__ npu_kv::ControlBlock *>(control);

    // Step 1: read the key the host staged. We don't reformat it -- the host
    // side already populated `ctrl->key`. We just print it so the run is
    // observable end-to-end.
    AscendC::printf("[NPU] kv_request_kernel: reqid=%s layerid=%u num_tokens=%u\n",
                    ctrl->key.reqid,
                    static_cast<unsigned>(ctrl->key.layerid),
                    static_cast<unsigned>(ctrl->key.num_tokens));

    // Step 2: NPU -> CPU. Tell the host we want it to service this Key.
    ctrl->flag = npu_kv::kFlagRequest;

    // Step 3: spin until the host writes kFlagResponse (or kFlagError).
    while (true) {
        std::uint32_t f = ctrl->flag;
        if (f == npu_kv::kFlagResponse) {
            break;
        }
        if (f == npu_kv::kFlagError) {
            AscendC::printf("[NPU] host reported error, aborting\n");
            return;
        }
    }

    // Step 4: consume.
    if (ctrl->status != npu_kv::kStatusOk) {
        AscendC::printf("[NPU] response status non-zero: %u\n",
                        static_cast<unsigned>(ctrl->status));
        ctrl->flag = npu_kv::kFlagDone;
        return;
    }
    AscendC::printf("[NPU] response: hits=%u misses=%u total=%u token_size=%u\n",
                    static_cast<unsigned>(ctrl->num_hits),
                    static_cast<unsigned>(ctrl->num_miss_tokens),
                    static_cast<unsigned>(ctrl->total_tokens),
                    static_cast<unsigned>(ctrl->token_size));

    // Sample-read first byte of each token so the path is exercised.
    auto *hbm = reinterpret_cast<__gm__ char *>(hbm_buf);
    for (std::uint32_t i = 0; i < ctrl->total_tokens; ++i) {
        char b = hbm[static_cast<std::size_t>(i) * ctrl->token_size];
        AscendC::printf("[NPU]   token[%u][0]=0x%02x\n",
                        static_cast<unsigned>(i),
                        static_cast<unsigned>(static_cast<unsigned char>(b)));
    }

    // Step 5: NPU -> CPU. Done.
    ctrl->flag = npu_kv::kFlagDone;
}

// Trampoline used by main.cpp on real NPU builds. The signature matches the
// pattern from samples/0_helloworld so it can be invoked with the same
// <<<...>>> launch syntax.
void kv_request_kernel_do(uint32_t blockDim,
                          void    *stream,
                          uint8_t *control,
                          uint8_t *hbm_buf)
{
    kv_request_kernel<<<blockDim, nullptr, stream>>>(control, hbm_buf);
}
