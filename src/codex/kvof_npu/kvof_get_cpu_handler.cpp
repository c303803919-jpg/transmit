#include "kvof_get_cpu_handler.h"

#include "kv_transfer.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace kv_transfer {
namespace kvof_npu {

bool process_kvof_get_request(RequestBlock* req) {
    if (req == nullptr || req->flag != kFlagRequest) {
        return false;
    }

    if (req->batch_size == 0 || req->batch_size > kMaxBatchSize ||
        req->num_segment > kMaxNumSegment) {
        req->flag = kFlagError;
        return false;
    }

    std::vector<std::string> req_storage;
    std::vector<const char*> req_ptrs;
    std::vector<std::int32_t> layerids;
    std::vector<std::int32_t> indices;

    req_storage.reserve(req->batch_size);
    req_ptrs.reserve(req->batch_size);
    layerids.reserve(req->batch_size);
    indices.reserve(static_cast<std::size_t>(req->batch_size) * req->num_segment);

    for (std::uint32_t b = 0; b < req->batch_size; ++b) {
        const std::size_t len = ::strnlen(req->req_lst[b], kMaxReqLen);
        if (len == kMaxReqLen) {
            req->flag = kFlagError;
            return false;
        }

        req_storage.emplace_back(req->req_lst[b], len);
        layerids.push_back(req->layerid_lst[b]);

        for (std::uint32_t s = 0; s < req->num_segment; ++s) {
            indices.push_back(req->index_lst[b][s]);
        }
    }

    for (const auto& reqid : req_storage) {
        req_ptrs.push_back(reqid.c_str());
    }

    const std::uint64_t kvof_id = kvof_get(req_ptrs.data(),
                                           layerids.data(),
                                           indices.data(),
                                           static_cast<int>(req->batch_size),
                                           static_cast<int>(req->segment_size),
                                           static_cast<int>(req->num_segment),
                                           static_cast<int>(req->src_media_type),
                                           static_cast<int>(req->dst_media_type));
    if (kvof_id == 0) {
        req->flag = kFlagError;
        return false;
    }

    req->kvof_id = kvof_id;
    req->flag = kFlagResponse;
    return true;
}

}  // namespace kvof_npu
}  // namespace kv_transfer
