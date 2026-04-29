#pragma once

#include <cstdint>

namespace kv_transfer {
namespace npu_ut {

constexpr std::uint32_t kFlagIdle = 0;
constexpr std::uint32_t kFlagRequest = 1;
constexpr std::uint32_t kFlagResponse = 2;
constexpr std::uint32_t kFlagDone = 3;
constexpr std::uint32_t kFlagError = 4;

constexpr std::uint32_t kMaxReqIdLen = 32;
constexpr std::uint32_t kMaxTokens = 16;
constexpr std::uint32_t kNpuRequestedTokens = 8;
constexpr std::uint32_t kTokenSizeBytes = 576;

struct RequestBlock {
    volatile std::uint32_t flag;
    char reqid[kMaxReqIdLen];
    std::uint32_t layerid;
    std::uint32_t num_tokens;
    std::uint32_t tokenids[kMaxTokens];
    std::uint32_t observed_first_bytes[kMaxTokens];
};

}  // namespace npu_ut
}  // namespace kv_transfer
