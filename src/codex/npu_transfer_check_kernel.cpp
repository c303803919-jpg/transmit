#include "kernel_operator.h"

extern "C" __global__ __aicore__ void kvof_transfer_check_kernel(GM_ADDR hbm,
                                                                  GM_ADDR status)
{
    if (AscendC::GetBlockIdx() != 0) {
        return;
    }

    auto* hbm_bytes = reinterpret_cast<__gm__ unsigned char*>(hbm);
    auto* out = reinterpret_cast<__gm__ unsigned int*>(status);

    AscendC::printf("[NPU] kvof_transfer_check_kernel is running on NPU\n");
    AscendC::printf("[NPU] HBM first bytes: %02x %02x %02x %02x\n",
                    static_cast<unsigned>(hbm_bytes[0]),
                    static_cast<unsigned>(hbm_bytes[1]),
                    static_cast<unsigned>(hbm_bytes[2]),
                    static_cast<unsigned>(hbm_bytes[3]));

    out[0] = static_cast<unsigned int>(hbm_bytes[0]);
    out[1] = static_cast<unsigned int>(hbm_bytes[1]);
    out[2] = static_cast<unsigned int>(hbm_bytes[2]);
    out[3] = static_cast<unsigned int>(hbm_bytes[3]);
}

void kvof_transfer_check_kernel_do(uint32_t blockDim,
                                   void* stream,
                                   uint8_t* hbm,
                                   uint8_t* status)
{
    kvof_transfer_check_kernel<<<blockDim, nullptr, stream>>>(hbm, status);
}
