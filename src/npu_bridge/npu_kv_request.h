// npu_kv_request.h
//
// Shared definitions used by both the NPU kernel (device) and the CPU host.
// The on-wire layout is a single ControlBlock followed by an HBM result area
// in one contiguous Global Memory (GM) allocation:
//
//   +-------------------+-----------------------------+
//   |   ControlBlock    |  HBM result area (tokens)   |
//   +-------------------+-----------------------------+
//
// The ControlBlock carries:
//   * KeyDevice  -- a fixed-size, POD mirror of kv_transfer::Key
//   * a volatile flag word used as a hand-shake between NPU and CPU
//   * the response metadata produced by the host service
//
// Keep this header header-only and dependency-free so it can be included
// from both Ascend C device code and ordinary host C++.

#pragma once

#include <cstdint>

namespace npu_kv {

// ---------- limits ----------
// Sizes are chosen so the whole control block fits in a small GM page.
constexpr std::uint32_t kMaxReqIdLen   = 64;
constexpr std::uint32_t kMaxTokens     = 64;
constexpr std::uint32_t kMaxMissGroups = 16;

// Token byte size *must* match kv_transfer::kTokenSize.
constexpr std::uint32_t kTokenSize        = 576;
constexpr std::uint32_t kHbmTokenCapacity = 256;
constexpr std::uint32_t kHbmAreaBytes     = kHbmTokenCapacity * kTokenSize;

// ---------- handshake states ----------
// All transitions are written to ControlBlock::flag.
//
//   host                                               npu kernel
//   ----                                               ----------
//   write KeyDevice                                    --
//   flag = kFlagIdle                                   --
//   launch kernel ------------------------------------> read key
//                                                       flag = kFlagRequest
//   <-- poll flag == kFlagRequest                       wait for kFlagResponse
//   3 metadata rounds (first/second/third)              ...
//   write hbm area, set num_hits/num_miss_tokens
//   flag = kFlagResponse  -------------------------->   consume data
//   <-- poll flag == kFlagDone                          flag = kFlagDone
//   tear down                                          (kernel exits)
enum FlagState : std::uint32_t {
    kFlagIdle     = 0,
    kFlagRequest  = 1,   // NPU -> CPU: please service this read
    kFlagResponse = 2,   // CPU -> NPU: data ready in hbm area
    kFlagDone     = 3,   // NPU -> CPU: data consumed, kernel exiting
    kFlagError    = 0xFFu,
};

// ---------- KeyDevice ----------
// Fixed-size, POD mirror of kv_transfer::Key (which uses std::string and
// std::vector and therefore cannot live in GM). The conversion is done by
// fill_key_device() / read_key_device() in npu_kv_host.h.
struct KeyDevice {
    char           reqid[kMaxReqIdLen];   // NUL-terminated
    std::uint32_t  layerid;
    std::uint32_t  num_tokens;
    std::uint32_t  tokenids[kMaxTokens];  // sorted ascending, like Key::tokenids
};

// ---------- ControlBlock ----------
// Lives at the start of the shared GM allocation. Layout is fixed; do not
// reorder without updating the kernel side as well.
struct ControlBlock {
    KeyDevice          key;

    // Hand-shake. `volatile` so reads/writes are not optimised away on either
    // side. The host additionally uses an explicit memory fence for ordering.
    volatile std::uint32_t flag;

    // ---- response, written by the CPU host ----
    std::uint32_t      num_hits;          // hit tokens at start of hbm area
    std::uint32_t      num_miss_tokens;   // miss tokens immediately after
    std::uint32_t      total_tokens;      // num_hits + num_miss_tokens
    std::uint32_t      token_size;        // bytes per token (== kTokenSize)
    std::uint32_t      status;            // 0 = success, non-zero = error code
    std::uint32_t      reserved[3];
};

// ---------- error codes ----------
enum StatusCode : std::uint32_t {
    kStatusOk              = 0,
    kStatusBadKey          = 1,
    kStatusFirstQueryFail  = 2,
    kStatusSecondQueryFail = 3,
    kStatusThirdQueryFail  = 4,
    kStatusHbmOverflow     = 5,
    kStatusHbmCopyFail     = 6,
};

// Total GM allocation size: ControlBlock + HBM result area.
constexpr std::size_t kSharedGmBytes =
    sizeof(ControlBlock) + static_cast<std::size_t>(kHbmAreaBytes);

}  // namespace npu_kv
