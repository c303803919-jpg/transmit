#include "kernel_operator.h"

#include "npu_token_get_index_request.h"

extern "C" __global__ __aicore__ void kvof_transfer_check_kernel(GM_ADDR request,
                                                                  GM_ADDR hbm)
{
    if (AscendC::GetBlockIdx() != 0) {
        return;
    }

    auto* req = reinterpret_cast<__gm__ kv_transfer::npu_ut::RequestBlock*>(
        request);
    auto* hbm_bytes = reinterpret_cast<__gm__ unsigned char*>(hbm);

    AscendC::printf("[NPU] issuing vector-key request on NPU\n");

    const char name[] = "npu-vector-key";
    for (unsigned int i = 0; i < sizeof(name); ++i) {
        req->reqid[i] = name[i];
    }
    req->layerid = 7;
    req->num_tokens = kv_transfer::npu_ut::kNpuRequestedTokens;
    for (unsigned int i = 0; i < kv_transfer::npu_ut::kNpuRequestedTokens; ++i) {
        req->tokenids[i] = i;
        req->observed_first_bytes[i] = 0;
    }

    req->flag = kv_transfer::npu_ut::kFlagRequest;

    while (true) {
        const unsigned int flag = req->flag;
        if (flag == kv_transfer::npu_ut::kFlagResponse) {
            break;
        }
        if (flag == kv_transfer::npu_ut::kFlagError) {
            AscendC::printf("[NPU] CPU reported error\n");
            return;
        }
    }

    AscendC::printf("[NPU] CPU finished token_get_index; checking 8 HBM slots\n");
    for (unsigned int i = 0; i < kv_transfer::npu_ut::kNpuRequestedTokens; ++i) {
        const unsigned char value =
            hbm_bytes[static_cast<unsigned long>(i) *
                      kv_transfer::npu_ut::kTokenSizeBytes];
        req->observed_first_bytes[i] = static_cast<unsigned int>(value);
        AscendC::printf("[NPU] hbm token[%u][0]=0x%02x expected=0x%02x\n",
                        i,
                        static_cast<unsigned>(value),
                        static_cast<unsigned>(req->tokenids[i] & 0xFF));
    }

    req->flag = kv_transfer::npu_ut::kFlagDone;
}

void kvof_transfer_check_kernel_do(uint32_t blockDim,
                                   void* stream,
                                   uint8_t* request,
                                   uint8_t* hbm)
{
    kvof_transfer_check_kernel<<<blockDim, nullptr, stream>>>(request, hbm);
}
