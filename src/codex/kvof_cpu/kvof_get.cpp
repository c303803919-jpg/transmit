// kvof_get.cpp
// CPU 侧入口实现：参数校验 -> 反序列化成 vector -> 循环调 token_get_index。

#include "kvof_get.h"

#include <atomic>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace kvof {

namespace {

// 单调自增的 kvof_ID 发号器；0 保留作为失败值。
std::atomic<std::uint64_t> g_next_kvof_id {1};

// 防御 NPU 侧传入未结束 string，避免 strlen 越界扫描。
constexpr std::size_t kMaxReqIdBytes = 4096;

}  // namespace

std::uint64_t kvof_get(const char* const* req_lst,
                       const std::int32_t* layerid_lst,
                       const std::int32_t* index_lst,
                       int batch_size,
                       int segment_size,
                       int num_segment,
                       int src_media_type,
                       int dst_media_type) {
    // ---- 参数校验 ----
    if (batch_size < 0 || num_segment < 0) {
        return 0;
    }
    // batch_size == 0 视为合法的空请求，直接发号返回。
    if (batch_size == 0) {
        return g_next_kvof_id.fetch_add(1, std::memory_order_relaxed);
    }
    if (req_lst == nullptr || layerid_lst == nullptr) {
        return 0;
    }
    if (num_segment > 0 && index_lst == nullptr) {
        return 0;
    }
    // 防 size_t 溢出： batch_size * num_segment
    const std::size_t b = static_cast<std::size_t>(batch_size);
    const std::size_t n = static_cast<std::size_t>(num_segment);
    if (n != 0 && b > std::numeric_limits<std::size_t>::max() / n) {
        return 0;
    }

    // ---- 反序列化成 vector<TokenGetIndexRequest> ----
    std::vector<TokenGetIndexRequest> reqs;
    try {
        reqs.reserve(b);
        for (int i = 0; i < batch_size; ++i) {
            if (req_lst[i] == nullptr) {
                return 0;
            }
            const std::size_t len = ::strnlen(req_lst[i], kMaxReqIdBytes);
            if (len == kMaxReqIdBytes) {
                return 0;  // 未截断到 NUL，视为非法
            }

            TokenGetIndexRequest req;
            req.reqid.assign(req_lst[i], len);
            req.layerid        = layerid_lst[i];
            req.segment_size   = segment_size;
            req.src_media_type = src_media_type;
            req.dst_media_type = dst_media_type;

            req.indices.reserve(n);
            const std::size_t row = static_cast<std::size_t>(i) * n;
            for (std::size_t j = 0; j < n; ++j) {
                req.indices.push_back(index_lst[row + j]);
            }

            reqs.push_back(std::move(req));
        }
    } catch (const std::bad_alloc&) {
        return 0;
    }

    // ---- 按 batch 调用 token_get_index ----
    for (const auto& req : reqs) {
        if (token_get_index(req) != 0) {
            return 0;
        }
    }

    return g_next_kvof_id.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace kvof
