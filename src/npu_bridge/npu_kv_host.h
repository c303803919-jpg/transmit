// npu_kv_host.h
//
// Host-side helpers for the CPU<->NPU KV-fetch hand-shake.
//
// The NPU kernel signals a read request via ControlBlock::flag; the host
// observes the request, runs the three metadata rounds (Round 1: hit/miss
// split, Round 2: hash + IP, Round 3: gather DRAM->HBM), writes the gathered
// data into the HBM area, and finally writes kFlagResponse so the kernel can
// proceed. The functions in this header implement that service path; they
// have no NPU dependency and can be unit-tested on a plain CPU host.

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include "../query/kv_transfer.h"
#include "npu_kv_request.h"

namespace npu_kv {

// Convert kv_transfer::Key -> KeyDevice (the GM-resident, fixed-size mirror).
// Returns false if the key would not fit (reqid too long or too many tokens).
bool fill_key_device(KeyDevice& kd, const kv_transfer::Key& k);

// Inverse of fill_key_device(): rebuild a kv_transfer::Key from the GM-resident
// representation so it can be fed to the existing kv_transfer::Client API.
kv_transfer::Key read_key_device(const KeyDevice& kd);

// Run the three-round metadata flow for the request currently sitting in
// `ctrl->key`, write the gathered tokens into `hbm_buf`, and update the
// response metadata fields of `*ctrl` (num_hits, num_miss_tokens,
// total_tokens, token_size, status).
//
//   * `hbm_capacity` is the size of `hbm_buf` in bytes (typically
//     kHbmAreaBytes).
//   * Returns true on success. On failure, ctrl->status carries the reason
//     and the function returns false; the caller is expected to flip
//     ctrl->flag to kFlagError so the kernel doesn't spin forever.
//
// The flag itself is *not* written by this function -- the caller orchestrates
// the hand-shake (see service_one_request() below).
bool run_three_round_query(ControlBlock* ctrl,
                           void*         hbm_buf,
                           std::size_t   hbm_capacity);

// Block until ControlBlock::flag matches `expected` or the timeout fires.
// Returns true if the flag was observed, false on timeout.
bool wait_for_flag(volatile std::uint32_t& flag,
                   std::uint32_t           expected,
                   std::chrono::milliseconds timeout =
                       std::chrono::milliseconds(5000));

// One-shot servicing helper:
//   1) wait for kFlagRequest
//   2) call run_three_round_query
//   3) write kFlagResponse on success / kFlagError on failure
//   4) wait for kFlagDone
//
// Returns true iff the full hand-shake completed cleanly. Designed to be
// called from the host main thread once the NPU kernel has been launched.
bool service_one_request(ControlBlock* ctrl,
                         void*         hbm_buf,
                         std::size_t   hbm_capacity,
                         std::chrono::milliseconds timeout =
                             std::chrono::milliseconds(5000));

}  // namespace npu_kv
