// npu_kv_host.cpp -- host-side service path for the CPU<->NPU hand-shake.
//
// The host:
//   1) reads the KeyDevice the kernel placed in GM,
//   2) issues Round 1 (first_query) -- splits hits vs. miss groups,
//   3) DRAM->HBM copy for hits,
//   4) issues Round 2 (second_query) -- hash + IP for each miss group,
//   5) dispatches local-vs-remote, then for the local set issues
//      Round 3 (third_query) and the gather DRAM->HBM copy,
//   6) updates the ControlBlock response fields and the hand-shake flag.

#include "npu_kv_host.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace npu_kv {

// ---------------------------------------------------------------------------
// Key marshalling
// ---------------------------------------------------------------------------

bool fill_key_device(KeyDevice& kd, const kv_transfer::Key& k) {
    std::memset(&kd, 0, sizeof(kd));
    if (k.reqid.size() >= kMaxReqIdLen) {
        return false;
    }
    if (k.tokenids.size() > kMaxTokens) {
        return false;
    }
    std::memcpy(kd.reqid, k.reqid.data(), k.reqid.size());
    kd.reqid[k.reqid.size()] = '\0';
    kd.layerid    = k.layerid;
    kd.num_tokens = static_cast<std::uint32_t>(k.tokenids.size());
    for (std::size_t i = 0; i < k.tokenids.size(); ++i) {
        kd.tokenids[i] = k.tokenids[i];
    }
    return true;
}

kv_transfer::Key read_key_device(const KeyDevice& kd) {
    kv_transfer::Key k;
    // reqid is NUL-terminated when produced by fill_key_device.
    k.reqid   = std::string(kd.reqid);
    k.layerid = kd.layerid;
    const std::uint32_t n =
        kd.num_tokens > kMaxTokens ? kMaxTokens : kd.num_tokens;
    k.tokenids.assign(kd.tokenids, kd.tokenids + n);
    return k;
}

// ---------------------------------------------------------------------------
// Three-round flow
// ---------------------------------------------------------------------------

bool run_three_round_query(ControlBlock* ctrl,
                           void*         hbm_buf,
                           std::size_t   hbm_capacity) {
    if (ctrl == nullptr || hbm_buf == nullptr) {
        if (ctrl) ctrl->status = kStatusBadKey;
        return false;
    }

    // 1) Build a kv_transfer::Key from the GM-resident KeyDevice.
    kv_transfer::Key key = read_key_device(ctrl->key);
    if (key.tokenids.size() > kMaxTokens) {
        ctrl->status = kStatusBadKey;
        return false;
    }

    kv_transfer::Client client;
    kv_transfer::Server server;

    // 2) Round 1 -- hit/miss split.
    kv_transfer::FirstQueryResult r1;
    try {
        r1 = client.first_query(key);
    } catch (...) {
        ctrl->status = kStatusFirstQueryFail;
        return false;
    }

    // 3) DRAM -> HBM for the hit list. Hits land at the start of hbm_buf.
    std::size_t hits_bytes = r1.hits.size() * kv_transfer::kTokenSize;
    if (hits_bytes > hbm_capacity) {
        ctrl->status = kStatusHbmOverflow;
        return false;
    }
    if (!client.process_hits(r1.hits, hbm_buf)) {
        ctrl->status = kStatusHbmCopyFail;
        return false;
    }

    // 4) Round 2 -- hash + IP per miss group.
    std::vector<kv_transfer::SecondQueryEntry> r2;
    try {
        r2 = client.second_query(r1.aggregated_key);
    } catch (...) {
        ctrl->status = kStatusSecondQueryFail;
        return false;
    }
    if (r2.size() != r1.miss_groups.size()) {
        ctrl->status = kStatusSecondQueryFail;
        return false;
    }

    // 5) Local-vs-remote dispatch. For local-IP entries, hand the matching
    //    miss group to the server side for Round 3 + gather DRAM->HBM.
    std::vector<std::uint64_t>           local_hashes;
    std::vector<kv_transfer::MissGroup>  local_groups;
    local_hashes.reserve(r2.size());
    local_groups.reserve(r2.size());
    for (std::size_t i = 0; i < r2.size(); ++i) {
        if (kv_transfer::Client::is_local_ip(r2[i].ip_address)) {
            local_hashes.push_back(r2[i].hash_key);
            local_groups.push_back(r1.miss_groups[i]);
        }
        // remote path: reserved -- caller would invoke client.remote_transfer().
    }

    std::size_t miss_token_count = 0;
    for (const auto& g : local_groups) {
        miss_token_count += g.offsets.size();
    }
    std::size_t miss_bytes = miss_token_count * kv_transfer::kTokenSize;
    if (hits_bytes + miss_bytes > hbm_capacity) {
        ctrl->status = kStatusHbmOverflow;
        return false;
    }

    // Round 3 + gather goes immediately after the hits.
    char* miss_dst = static_cast<char*>(hbm_buf) + hits_bytes;
    if (!server.handle_transfer_request(local_hashes, local_groups, miss_dst)) {
        ctrl->status = kStatusThirdQueryFail;
        return false;
    }

    // 6) Update response metadata.
    ctrl->num_hits        = static_cast<std::uint32_t>(r1.hits.size());
    ctrl->num_miss_tokens = static_cast<std::uint32_t>(miss_token_count);
    ctrl->total_tokens    = ctrl->num_hits + ctrl->num_miss_tokens;
    ctrl->token_size      = static_cast<std::uint32_t>(kv_transfer::kTokenSize);
    ctrl->status          = kStatusOk;
    return true;
}

// ---------------------------------------------------------------------------
// Hand-shake helpers
// ---------------------------------------------------------------------------

bool wait_for_flag(volatile std::uint32_t&    flag,
                   std::uint32_t              expected,
                   std::chrono::milliseconds  timeout) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + timeout;
    for (;;) {
        // Acquire fence pairs with the writer's release fence on the other side
        // so we don't see torn or stale flag writes.
        std::atomic_thread_fence(std::memory_order_acquire);
        std::uint32_t cur = flag;
        if (cur == expected) {
            return true;
        }
        if (clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

bool service_one_request(ControlBlock*             ctrl,
                         void*                     hbm_buf,
                         std::size_t               hbm_capacity,
                         std::chrono::milliseconds timeout) {
    if (ctrl == nullptr || hbm_buf == nullptr) {
        return false;
    }

    // 1) Wait for the kernel to raise the request.
    if (!wait_for_flag(ctrl->flag, kFlagRequest, timeout)) {
        std::cerr << "[Host] timed out waiting for kFlagRequest\n";
        return false;
    }

    // 2) Run the three-round flow into the HBM area.
    const bool ok = run_three_round_query(ctrl, hbm_buf, hbm_capacity);

    // 3) Publish data + flag with a release fence so the kernel observes the
    //    response payload before it observes the flag transition.
    std::atomic_thread_fence(std::memory_order_release);
    ctrl->flag = ok ? kFlagResponse : kFlagError;

    if (!ok) {
        std::cerr << "[Host] run_three_round_query failed (status="
                  << ctrl->status << ")\n";
        return false;
    }

    // 4) Wait for the kernel to acknowledge consumption.
    if (!wait_for_flag(ctrl->flag, kFlagDone, timeout)) {
        std::cerr << "[Host] timed out waiting for kFlagDone\n";
        return false;
    }
    return true;
}

}  // namespace npu_kv
