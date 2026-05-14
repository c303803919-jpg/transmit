// kvof_get.h
// 华为昇腾 910c NPU 通过 AICPU / aclrtLaunchCallback 通知 CPU 后，
// CPU 侧执行的入口函数。把 NPU 传过来的裸 buffer 反序列化成 vector，
// 然后按 batch 调用 token_get_index，处理完返回 kvof_ID 给 NPU。
//
// NPU 侧伪代码（仅说明，本文件不实现）：
//   // device 端备好 req/layerid/index 三个连续 buffer
//   aclrtLaunchCallback(kvof_get_callback, payload, ACL_CALLBACK_BLOCK, stream);
//   // 在 callback 里调用本头声明的 kvof_get(...)，拿到 kvof_id 后通过
//   // event/notify 回到 NPU。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kvof {

// 一个 batch 元素被反序列化后的形态。
struct TokenGetIndexRequest {
    std::string                reqid;             // 来自 req_lst[i]
    std::int32_t               layerid {0};       // 来自 layerid_lst[i]
    std::vector<std::int32_t>  indices;           // 来自 index_lst[i][*]
    std::int32_t               segment_size {0};
    std::int32_t               src_media_type {0};
    std::int32_t               dst_media_type {0};
};

// 由使用方提供的 per-request 处理器。
// 返回 0 表示成功，非 0 表示失败（kvof_get 会立即中止并返回 0）。
int token_get_index(const TokenGetIndexRequest& req);

// NPU -> CPU 桥接入口。
//   req_lst       : 长度 batch_size 的 C 字符串数组（不定长 string）
//   layerid_lst   : 长度 batch_size 的 int32 数组
//   index_lst     : 行优先的 [batch_size, num_segment] int32 二维数组
//   batch_size    : 批大小
//   segment_size  : 每个 segment 的字节数（透传给 token_get_index）
//   num_segment   : 每个请求的 segment 数（即 index_lst 的列数）
//   src_media_type / dst_media_type : 透传的搬移源/目的介质类型
//
// 返回：成功返回非 0 的 kvof_ID（单调自增），任意校验或处理失败返回 0。
std::uint64_t kvof_get(const char* const* req_lst,
                       const std::int32_t* layerid_lst,
                       const std::int32_t* index_lst,
                       int batch_size,
                       int segment_size,
                       int num_segment,
                       int src_media_type,
                       int dst_media_type);

}  // namespace kvof
