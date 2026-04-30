#include "acl/acl.h"
#include "kvof_cpu/kv_transfer.h"
#include "npu_token_get_index_request.h"

#include <chrono>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

extern void kvof_transfer_check_kernel_do(uint32_t blockDim,
                                          void* stream,
                                          uint8_t* request,
                                          uint8_t* hbm);

namespace {

#define CHECK_ACL(x)                                                     \
    do {                                                                 \
        std::cout << "[CPU] begin " << #x << std::endl;                  \
        aclError ret = (x);                                              \
        if (ret != ACL_SUCCESS) {                                        \
            std::cerr << "[CPU] " << #x << " failed, ret=" << ret        \
                      << std::endl;                                      \
            return 1;                                                    \
        }                                                                \
        std::cout << "[CPU] done  " << #x << std::endl;                  \
    } while (0)

}  // namespace

int main() {
    std::cout.setf(std::ios::unitbuf);
    std::cout << "[CPU] npu_token_get_index_ut starts on CPU host" << std::endl;

    CHECK_ACL(aclInit(nullptr));
    std::cout << "[CPU] aclInit finished; creating runtime context" << std::endl;
    int32_t device_id = 0;
    CHECK_ACL(aclrtSetDevice(device_id));
    std::cout << "[CPU] aclrtSetDevice finished, device_id=" << device_id
              << std::endl;

    aclrtStream stream = nullptr;
    CHECK_ACL(aclrtCreateStream(&stream));
    std::cout << "[CPU] aclrtCreateStream finished, stream=" << stream
              << std::endl;

    void* hbm = nullptr;
    CHECK_ACL(aclrtMalloc(&hbm,
                          kv_transfer::kTokenSize *
                              kv_transfer::npu_ut::kNpuRequestedTokens,
                          ACL_MEM_MALLOC_HUGE_FIRST));
    std::cout << "[CPU] HBM allocation finished, hbm=" << hbm
              << " bytes="
              << kv_transfer::kTokenSize *
                     kv_transfer::npu_ut::kNpuRequestedTokens
              << std::endl;

    void* request_dev = nullptr;
    CHECK_ACL(aclrtMalloc(&request_dev,
                          sizeof(kv_transfer::npu_ut::RequestBlock),
                          ACL_MEM_MALLOC_HUGE_FIRST));
    std::cout << "[CPU] request block allocation finished, request_dev="
              << request_dev << " bytes="
              << sizeof(kv_transfer::npu_ut::RequestBlock) << std::endl;

    auto* request = reinterpret_cast<kv_transfer::npu_ut::RequestBlock*>(
        request_dev);
    std::memset(request, 0, sizeof(*request));
    request->flag = kv_transfer::npu_ut::kFlagIdle;

    constexpr uint32_t block_dim = 1;
    std::cout << "[CPU] launching NPU kernel first; NPU will create the vector "
                 "key request" << std::endl;
    kvof_transfer_check_kernel_do(
        block_dim, stream,
        reinterpret_cast<uint8_t*>(request_dev),
        reinterpret_cast<uint8_t*>(hbm));
    std::cout << "[CPU] NPU kernel launch returned on host; polling request flag"
              << std::endl;

    bool got_request = false;
    for (int retry = 0; retry < 10000; ++retry) {
        std::atomic_thread_fence(std::memory_order_acquire);
        if (request->flag == kv_transfer::npu_ut::kFlagRequest) {
            got_request = true;
            break;
        }
        if ((retry % 1000) == 0) {
            std::cout << "[CPU] waiting for NPU request, retry=" << retry
                      << " flag=" << request->flag << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    if (!got_request) {
        std::cerr << "[CPU] timed out waiting for NPU vector-key request"
                  << std::endl;
        return 1;
    }

    std::cout << "[CPU] received request from NPU: reqid="
              << request->reqid
              << " layerid=" << request->layerid
              << " tokens=" << request->num_tokens << std::endl;

    kv_transfer::Key key;
    key.reqid = request->reqid;
    key.layerid = request->layerid;
    for (std::uint32_t i = 0; i < request->num_tokens; ++i) {
        key.tokenids.push_back(request->tokenids[i]);
    }

    std::cout << "[CPU] vector key from NPU:";
    for (std::uint32_t tokenid : key.tokenids) {
        std::cout << ' ' << tokenid;
    }
    std::cout << std::endl;

    kv_transfer::Kvof kvof(
        hbm, kv_transfer::kTokenSize * kv_transfer::npu_ut::kNpuRequestedTokens);

    std::cout << "[CPU] calling Kvof::token_get_index on CPU host; metadata "
                 "query stays on CPU, data movement uses aclrtMemcpy"
              << std::endl;
    if (!kvof.token_get_index(key)) {
        std::cerr << "[CPU] Kvof::token_get_index failed" << std::endl;
        std::atomic_thread_fence(std::memory_order_release);
        request->flag = kv_transfer::npu_ut::kFlagError;
        return 1;
    }

    std::atomic_thread_fence(std::memory_order_release);
    request->flag = kv_transfer::npu_ut::kFlagResponse;

    std::cout << "[CPU] response flag sent; waiting for NPU to verify all "
                 "8 HBM token slots" << std::endl;
    CHECK_ACL(aclrtSynchronizeStream(stream));

    std::atomic_thread_fence(std::memory_order_acquire);
    if (request->flag != kv_transfer::npu_ut::kFlagDone) {
        std::cerr << "[CPU] NPU did not finish successfully, flag="
                  << request->flag << std::endl;
        return 1;
    }

    std::cout << "[CPU] NPU observed first byte per token:";
    for (std::uint32_t i = 0; i < request->num_tokens; ++i) {
        std::cout << " token[" << i << "]=0x"
                  << std::hex << std::setw(2) << std::setfill('0')
                  << request->observed_first_bytes[i]
                  << std::dec << std::setfill(' ');
    }
    std::cout << std::endl;

    for (std::uint32_t i = 0; i < request->num_tokens; ++i) {
        const std::uint32_t expected = request->tokenids[i] & 0xFF;
        if (request->observed_first_bytes[i] != expected) {
            std::cerr << "[CPU] mismatch token[" << i << "]: observed=0x"
                      << std::hex << request->observed_first_bytes[i]
                      << " expected=0x" << expected << std::dec << std::endl;
            return 1;
        }
    }

    CHECK_ACL(aclrtFree(request_dev));
    CHECK_ACL(aclrtFree(hbm));
    CHECK_ACL(aclrtDestroyStream(stream));
    CHECK_ACL(aclrtResetDevice(device_id));
    CHECK_ACL(aclFinalize());

    std::cout << "[CPU] npu_token_get_index_ut passed" << std::endl;
    return 0;
}
