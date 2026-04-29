#include "acl/acl.h"
#include "kv_transfer.h"

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <vector>

extern void kvof_transfer_check_kernel_do(uint32_t blockDim,
                                          void* stream,
                                          uint8_t* hbm,
                                          uint8_t* status);

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

}  // namespace

int main() {
    std::cout << "[CPU] npu_token_get_index_ut starts on CPU host\n";

    CHECK_ACL(aclInit(nullptr));
    int32_t device_id = 0;
    CHECK_ACL(aclrtSetDevice(device_id));

    aclrtStream stream = nullptr;
    CHECK_ACL(aclrtCreateStream(&stream));

    void* hbm = nullptr;
    CHECK_ACL(aclrtMalloc(&hbm, kv_transfer::kTokenSize * 4,
                          ACL_MEM_MALLOC_HUGE_FIRST));

    void* status_dev = nullptr;
    CHECK_ACL(aclrtMalloc(&status_dev, 4 * sizeof(unsigned int),
                          ACL_MEM_MALLOC_HUGE_FIRST));

    alignas(64) unsigned char src[kv_transfer::kTokenSize] = {};
    for (std::size_t i = 0; i < kv_transfer::kTokenSize; ++i) {
        src[i] = static_cast<unsigned char>((0xC0 + i) & 0xFF);
    }

    kv_transfer::Kvof kvof(hbm, kv_transfer::kTokenSize * 4);

    kv_transfer::Key key;
    key.reqid = "npu-ut";
    key.layerid = 1;
    key.tokenids = {0, 1};

    std::cout << "[CPU] calling Kvof::token_get_index on CPU host; metadata "
                 "query stays on CPU, data movement uses aclrtMemcpy\n";
    if (!kvof.token_get_index(key)) {
        std::cerr << "[CPU] Kvof::token_get_index failed\n";
        return 1;
    }

    print_bytes("[CPU] source bytes before va_to_hbm:", src, 8);
    std::cout << "[CPU] calling Kvof::va_to_hbm on CPU host; it uses aclrtMemcpy "
                 "HOST_TO_DEVICE\n";
    if (kvof.va_to_hbm(src, 0) != 0) {
        std::cerr << "[CPU] Kvof::va_to_hbm failed\n";
        return 1;
    }

    constexpr uint32_t block_dim = 1;
    std::cout << "[CPU] launching NPU kernel now; look for [NPU] prints below\n";
    kvof_transfer_check_kernel_do(
        block_dim, stream,
        reinterpret_cast<uint8_t*>(hbm),
        reinterpret_cast<uint8_t*>(status_dev));
    CHECK_ACL(aclrtSynchronizeStream(stream));

    unsigned int status_host[4] = {};
    CHECK_ACL(aclrtMemcpy(status_host, sizeof(status_host),
                          status_dev, sizeof(status_host),
                          ACL_MEMCPY_DEVICE_TO_HOST));

    std::cout << "[CPU] bytes observed by NPU kernel:";
    for (unsigned int value : status_host) {
        std::cout << " 0x" << std::hex << std::setw(2) << std::setfill('0')
                  << value;
    }
    std::cout << std::dec << std::setfill(' ') << '\n';

    const bool ok = status_host[0] == src[0] &&
                    status_host[1] == src[1] &&
                    status_host[2] == src[2] &&
                    status_host[3] == src[3];
    if (!ok) {
        std::cerr << "[CPU] NPU-observed bytes do not match source bytes\n";
        return 1;
    }

    CHECK_ACL(aclrtFree(status_dev));
    CHECK_ACL(aclrtFree(hbm));
    CHECK_ACL(aclrtDestroyStream(stream));
    CHECK_ACL(aclrtResetDevice(device_id));
    CHECK_ACL(aclFinalize());

    std::cout << "[CPU] npu_token_get_index_ut passed\n";
    return 0;
}
