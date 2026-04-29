#include "acl/acl.h"
#include "kv_transfer.h"
#include "npu_token_get_index_request.h"

#include <chrono>
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
        aclError ret = (x);                                              \
        if (ret != ACL_SUCCESS) {                                        \
            std::cerr << "[CPU] " << #x << " failed, ret=" << ret        \
                      << '\n';                                           \
            return 1;                                                    \
        }                                                                \
    } while (0)

void print_bytes(const char* label, const unsigned char* data, std::size_t n) {
    std::cout << label;
    for (std::size_t i = 0; i < n; ++i) {
        std::cout << " 0x" << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(data[i]);
    }
    std::cout << std::dec << std::setfill(' ') << '\n';
}

bool copy_request_from_device(void* request_dev,
                              kv_transfer::npu_ut::RequestBlock& host) {
    return aclrtMemcpy(&host, sizeof(host), request_dev, sizeof(host),
                       ACL_MEMCPY_DEVICE_TO_HOST) == ACL_SUCCESS;
}

}  // namespace

int main() {
    std::cout << "[CPU] npu_token_get_index_ut starts on CPU host\n";

    CHECK_ACL(aclInit(nullptr));
    int32_t device_id = 0;
    CHECK_ACL(aclrtSetDevice(device_id));

    aclrtStream stream = nullptr;
    CHECK_ACL(aclrtCreateStream(&stream));

    void* hbm = nullptr;
    CHECK_ACL(aclrtMalloc(&hbm,
                          kv_transfer::kTokenSize *
                              kv_transfer::npu_ut::kNpuRequestedTokens,
                          ACL_MEM_MALLOC_HUGE_FIRST));

    void* request_dev = nullptr;
    CHECK_ACL(aclrtMalloc(&request_dev,
                          sizeof(kv_transfer::npu_ut::RequestBlock),
                          ACL_MEM_MALLOC_HUGE_FIRST));

    kv_transfer::npu_ut::RequestBlock request_host{};
    CHECK_ACL(aclrtMemcpy(request_dev, sizeof(request_host),
                          &request_host, sizeof(request_host),
                          ACL_MEMCPY_HOST_TO_DEVICE));

    constexpr uint32_t block_dim = 1;
    std::cout << "[CPU] launching NPU kernel first; NPU will create the vector "
                 "key request\n";
    kvof_transfer_check_kernel_do(
        block_dim, stream,
        reinterpret_cast<uint8_t*>(request_dev),
        reinterpret_cast<uint8_t*>(hbm));

    bool got_request = false;
    for (int retry = 0; retry < 10000; ++retry) {
        if (!copy_request_from_device(request_dev, request_host)) {
            std::cerr << "[CPU] failed to copy request from device\n";
            return 1;
        }
        if (request_host.flag == kv_transfer::npu_ut::kFlagRequest) {
            got_request = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    if (!got_request) {
        std::cerr << "[CPU] timed out waiting for NPU vector-key request\n";
        return 1;
    }

    std::cout << "[CPU] received request from NPU: reqid="
              << request_host.reqid
              << " layerid=" << request_host.layerid
              << " tokens=" << request_host.num_tokens << '\n';

    kv_transfer::Key key;
    key.reqid = request_host.reqid;
    key.layerid = request_host.layerid;
    for (std::uint32_t i = 0; i < request_host.num_tokens; ++i) {
        key.tokenids.push_back(request_host.tokenids[i]);
    }

    std::cout << "[CPU] vector key from NPU:";
    for (std::uint32_t tokenid : key.tokenids) {
        std::cout << ' ' << tokenid;
    }
    std::cout << '\n';

    kv_transfer::Kvof kvof(
        hbm, kv_transfer::kTokenSize * kv_transfer::npu_ut::kNpuRequestedTokens);

    std::cout << "[CPU] calling Kvof::token_get_index on CPU host; metadata "
                 "query stays on CPU, data movement uses aclrtMemcpy\n";
    if (!kvof.token_get_index(key)) {
        std::cerr << "[CPU] Kvof::token_get_index failed\n";
        request_host.flag = kv_transfer::npu_ut::kFlagError;
        aclrtMemcpy(request_dev, sizeof(request_host),
                    &request_host, sizeof(request_host),
                    ACL_MEMCPY_HOST_TO_DEVICE);
        return 1;
    }

    request_host.flag = kv_transfer::npu_ut::kFlagResponse;
    CHECK_ACL(aclrtMemcpy(request_dev, sizeof(request_host),
                          &request_host, sizeof(request_host),
                          ACL_MEMCPY_HOST_TO_DEVICE));

    std::cout << "[CPU] response flag sent; waiting for NPU to verify all "
                 "8 HBM token slots\n";
    CHECK_ACL(aclrtSynchronizeStream(stream));

    CHECK_ACL(aclrtMemcpy(&request_host, sizeof(request_host),
                          request_dev, sizeof(request_host),
                          ACL_MEMCPY_DEVICE_TO_HOST));

    if (request_host.flag != kv_transfer::npu_ut::kFlagDone) {
        std::cerr << "[CPU] NPU did not finish successfully, flag="
                  << request_host.flag << '\n';
        return 1;
    }

    std::cout << "[CPU] NPU observed first byte per token:";
    for (std::uint32_t i = 0; i < request_host.num_tokens; ++i) {
        std::cout << " token[" << i << "]=0x"
                  << std::hex << std::setw(2) << std::setfill('0')
                  << request_host.observed_first_bytes[i]
                  << std::dec << std::setfill(' ');
    }
    std::cout << '\n';

    for (std::uint32_t i = 0; i < request_host.num_tokens; ++i) {
        const std::uint32_t expected = request_host.tokenids[i] & 0xFF;
        if (request_host.observed_first_bytes[i] != expected) {
            std::cerr << "[CPU] mismatch token[" << i << "]: observed=0x"
                      << std::hex << request_host.observed_first_bytes[i]
                      << " expected=0x" << expected << std::dec << '\n';
            return 1;
        }
    }

    CHECK_ACL(aclrtFree(request_dev));
    CHECK_ACL(aclrtFree(hbm));
    CHECK_ACL(aclrtDestroyStream(stream));
    CHECK_ACL(aclrtResetDevice(device_id));
    CHECK_ACL(aclFinalize());

    std::cout << "[CPU] npu_token_get_index_ut passed\n";
    return 0;
}
