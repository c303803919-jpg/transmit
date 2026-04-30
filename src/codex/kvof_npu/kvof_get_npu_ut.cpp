#include "acl/acl.h"

#include "kvof_get_cpu_handler.h"
#include "kvof_npu_request.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>

extern void kvof_get_notify_kernel_do(uint32_t block_dim,
                                      void* stream,
                                      uint8_t* request);

namespace {

#define CHECK_ACL(x)                                                     \
    do {                                                                 \
        aclError ret = (x);                                              \
        if (ret != ACL_SUCCESS) {                                        \
            std::cerr << #x << " failed, ret=" << ret << std::endl;      \
            return 1;                                                    \
        }                                                                \
    } while (0)

}  // namespace

int main() {
    CHECK_ACL(aclInit(nullptr));

    const int32_t device_id = 0;
    CHECK_ACL(aclrtSetDevice(device_id));

    aclrtStream stream = nullptr;
    CHECK_ACL(aclrtCreateStream(&stream));

    void* request_dev = nullptr;
    CHECK_ACL(aclrtMalloc(&request_dev,
                          sizeof(kv_transfer::kvof_npu::RequestBlock),
                          ACL_MEM_MALLOC_HUGE_FIRST));

    kv_transfer::kvof_npu::RequestBlock request_host {};
    std::memset(&request_host, 0, sizeof(request_host));
    request_host.flag = kv_transfer::kvof_npu::kFlagIdle;

    if (aclrtMemcpy(request_dev,
                    sizeof(request_host),
                    &request_host,
                    sizeof(request_host),
                    ACL_MEMCPY_HOST_TO_DEVICE) != ACL_SUCCESS) {
        std::cerr << "Initial host-to-device memcpy failed" << std::endl;
        (void)aclrtFree(request_dev);
        (void)aclrtDestroyStream(stream);
        (void)aclrtResetDevice(device_id);
        (void)aclFinalize();
        return 1;
    }

    const auto cleanup = [&]() {
        if (request_dev != nullptr) {
            (void)aclrtFree(request_dev);
            request_dev = nullptr;
        }
        if (stream != nullptr) {
            (void)aclrtDestroyStream(stream);
            stream = nullptr;
        }
        (void)aclrtResetDevice(device_id);
        (void)aclFinalize();
    };

    kvof_get_notify_kernel_do(1,
                              stream,
                              reinterpret_cast<uint8_t*>(request_dev));

    bool got_request = false;
    constexpr int kMaxPollRetries = 10000;
    for (int retry = 0; retry < kMaxPollRetries; ++retry) {
        if (aclrtMemcpy(&request_host,
                        sizeof(request_host),
                        request_dev,
                        sizeof(request_host),
                        ACL_MEMCPY_DEVICE_TO_HOST) != ACL_SUCCESS) {
            std::cerr << "Device-to-host memcpy failed while polling request"
                      << std::endl;
            cleanup();
            return 1;
        }

        if (request_host.flag == kv_transfer::kvof_npu::kFlagRequest) {
            got_request = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    if (!got_request) {
        // Print the last observed flag so it's clear whether the NPU kernel
        // never started (flag==kFlagIdle) or is stuck mid-execution.
        std::cerr << "Timed out waiting for kFlagRequest after "
                  << kMaxPollRetries << " retries; "
                  << "last flag=" << request_host.flag << std::endl;
        cleanup();
        return 1;
    }

    if (!kv_transfer::kvof_npu::process_kvof_get_request(&request_host)) {
        // handler sets flag=kFlagError; print it to distinguish validation
        // failures (bad shape, bad req string) from kvof_get() returning 0.
        std::cerr << "CPU failed to process kvof_get request, "
                  << "flag after call=" << request_host.flag << std::endl;
        cleanup();
        return 1;
    }

    if (aclrtMemcpy(request_dev,
                    sizeof(request_host),
                    &request_host,
                    sizeof(request_host),
                    ACL_MEMCPY_HOST_TO_DEVICE) != ACL_SUCCESS) {
        std::cerr << "Host-to-device memcpy failed after CPU processing"
                  << std::endl;
        cleanup();
        return 1;
    }

    if (aclrtSynchronizeStream(stream) != ACL_SUCCESS) {
        std::cerr << "aclrtSynchronizeStream failed" << std::endl;
        cleanup();
        return 1;
    }

    if (aclrtMemcpy(&request_host,
                    sizeof(request_host),
                    request_dev,
                    sizeof(request_host),
                    ACL_MEMCPY_DEVICE_TO_HOST) != ACL_SUCCESS) {
        std::cerr << "Final device-to-host memcpy failed" << std::endl;
        cleanup();
        return 1;
    }

    if (request_host.flag != kv_transfer::kvof_npu::kFlagDone) {
        std::cerr << "NPU did not complete, flag=" << request_host.flag
                  << std::endl;
        cleanup();
        return 1;
    }

    if (request_host.kvof_id == 0) {
        std::cerr << "Invalid kvof_id=0" << std::endl;
        cleanup();
        return 1;
    }

    std::cout << "kvof_get_npu_ut passed, kvof_id=" << request_host.kvof_id
              << std::endl;

    cleanup();
    return 0;
}
