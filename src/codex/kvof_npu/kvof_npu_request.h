#pragma once

#include <cstdint>

namespace kv_transfer {
namespace kvof_npu {

constexpr std::uint32_t kFlagIdle = 0;
constexpr std::uint32_t kFlagRequest = 1;
constexpr std::uint32_t kFlagResponse = 2;
constexpr std::uint32_t kFlagDone = 3;
constexpr std::uint32_t kFlagError = 4;

constexpr std::uint32_t kMaxBatchSize = 8;
constexpr std::uint32_t kMaxReqLen = 32;
constexpr std::uint32_t kMaxNumSegment = 16;

struct RequestBlock {
    volatile std::uint32_t flag;
    std::uint64_t kvof_id;

    std::uint32_t batch_size;
    std::uint32_t segment_size;
    std::uint32_t num_segment;
    std::uint32_t src_media_type;
    std::uint32_t dst_media_type;

    char req_lst[kMaxBatchSize][kMaxReqLen];
    std::int32_t layerid_lst[kMaxBatchSize];
    std::int32_t index_lst[kMaxBatchSize][kMaxNumSegment];
};

}  // namespace kvof_npu
}  // namespace kv_transfer
