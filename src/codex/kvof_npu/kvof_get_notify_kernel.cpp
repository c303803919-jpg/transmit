#include "kernel_operator.h"

#include "kvof_npu_request.h"

extern "C" __global__ __aicore__ void kvof_get_notify_kernel(GM_ADDR request) {
    if (AscendC::GetBlockIdx() != 0) {
        return;
    }

    auto* req = reinterpret_cast<__gm__ kv_transfer::kvof_npu::RequestBlock*>(request);

    req->kvof_id = 0;
    req->batch_size = 2;
    req->segment_size = 1;
    req->num_segment = 4;
    req->src_media_type = 0;
    req->dst_media_type = 1;

    req->req_lst[0][0] = 'n';
    req->req_lst[0][1] = 'p';
    req->req_lst[0][2] = 'u';
    req->req_lst[0][3] = '-';
    req->req_lst[0][4] = 'r';
    req->req_lst[0][5] = 'e';
    req->req_lst[0][6] = 'q';
    req->req_lst[0][7] = '-';
    req->req_lst[0][8] = '0';
    req->req_lst[0][9] = '\0';

    req->req_lst[1][0] = 'n';
    req->req_lst[1][1] = 'p';
    req->req_lst[1][2] = 'u';
    req->req_lst[1][3] = '-';
    req->req_lst[1][4] = 'r';
    req->req_lst[1][5] = 'e';
    req->req_lst[1][6] = 'q';
    req->req_lst[1][7] = '-';
    req->req_lst[1][8] = '1';
    req->req_lst[1][9] = '\0';

    req->layerid_lst[0] = 7;
    req->layerid_lst[1] = 8;

    for (unsigned int b = 0; b < req->batch_size; ++b) {
        for (unsigned int s = 0; s < req->num_segment; ++s) {
            req->index_lst[b][s] = static_cast<int>((b + 1) * 100 + s);
        }
    }

    AscendC::printf("[NPU] kvof_get request ready, notify CPU\n");
    req->flag = kv_transfer::kvof_npu::kFlagRequest;

    while (true) {
        const unsigned int flag = req->flag;
        if (flag == kv_transfer::kvof_npu::kFlagResponse) {
            break;
        }
        if (flag == kv_transfer::kvof_npu::kFlagError) {
            AscendC::printf("[NPU] CPU handler failed\n");
            return;
        }
    }

    AscendC::printf("[NPU] CPU done, kvof_id=%llu\n", req->kvof_id);
    req->flag = kv_transfer::kvof_npu::kFlagDone;
}

void kvof_get_notify_kernel_do(uint32_t block_dim,
                               void* stream,
                               uint8_t* request) {
    kvof_get_notify_kernel<<<block_dim, nullptr, stream>>>(request);
}
