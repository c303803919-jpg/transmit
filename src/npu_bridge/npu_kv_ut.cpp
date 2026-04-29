// npu_kv_ut.cpp
//
// Unit tests for the CPU<->NPU KV hand-shake. Uses the CPU simulator
// (npu_kv_sim.cpp) in place of the real Ascend C kernel so the tests can run
// on any host. Style matches src/query/kv_transfer_ut.cpp (lightweight,
// no gtest dependency).

#include "npu_kv_host.h"
#include "npu_kv_request.h"
#include "npu_kv_sim.h"
#include "../query/kv_transfer.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

using namespace npu_kv;

namespace {

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::fprintf(stderr, "FAIL: %s @ %s:%d\n",                        \
                         #cond, __FILE__, __LINE__);                          \
            std::exit(1);                                                     \
        }                                                                     \
    } while (0)

#define RUN(test_fn)                                  \
    do {                                              \
        std::cout << "[ RUN  ] " #test_fn << '\n';    \
        test_fn();                                    \
        std::cout << "[  OK  ] " #test_fn << '\n';    \
    } while (0)

// One backing buffer per test, sized to match the production GM allocation.
struct SharedRegion {
    alignas(64) unsigned char bytes[kSharedGmBytes] = {};

    ControlBlock* ctrl()  { return reinterpret_cast<ControlBlock*>(bytes); }
    void*         hbm()   { return bytes + sizeof(ControlBlock); }
    std::size_t   hbm_capacity() const { return kHbmAreaBytes; }
};

void make_default_key(kv_transfer::Key& k) {
    k.reqid    = "req-001";
    k.layerid  = 7;
    // even -> hit, odd -> miss; gives a mix of hits and miss groups.
    k.tokenids = {0, 1, 2, 3, 4, 5, 8, 17, 19, 33};
}

// ---------------------------------------------------------------------------
// 1) Key marshalling round-trip
// ---------------------------------------------------------------------------

void test_fill_and_read_key_device() {
    kv_transfer::Key k;
    make_default_key(k);

    KeyDevice kd{};
    EXPECT(fill_key_device(kd, k));
    EXPECT(std::string(kd.reqid) == "req-001");
    EXPECT(kd.layerid == 7u);
    EXPECT(kd.num_tokens == k.tokenids.size());
    for (std::size_t i = 0; i < k.tokenids.size(); ++i) {
        EXPECT(kd.tokenids[i] == k.tokenids[i]);
    }

    auto k2 = read_key_device(kd);
    EXPECT(k2.reqid == k.reqid);
    EXPECT(k2.layerid == k.layerid);
    EXPECT(k2.tokenids == k.tokenids);
}

void test_fill_key_device_rejects_oversized() {
    kv_transfer::Key k;
    k.reqid = std::string(kMaxReqIdLen, 'x');   // too long (no NUL room)
    KeyDevice kd{};
    EXPECT(!fill_key_device(kd, k));

    k.reqid = "ok";
    k.tokenids.assign(kMaxTokens + 1, 0);
    EXPECT(!fill_key_device(kd, k));
}

// ---------------------------------------------------------------------------
// 2) run_three_round_query writes the right shape
// ---------------------------------------------------------------------------

void test_run_three_round_query_layout() {
    SharedRegion region;
    auto* ctrl = region.ctrl();

    kv_transfer::Key k;
    make_default_key(k);
    EXPECT(fill_key_device(ctrl->key, k));
    ctrl->flag = kFlagIdle;

    EXPECT(run_three_round_query(ctrl, region.hbm(), region.hbm_capacity()));
    EXPECT(ctrl->status == kStatusOk);
    EXPECT(ctrl->token_size == kv_transfer::kTokenSize);

    // Even tokens are hits in the kv_transfer stub: {0,2,4,8} -> 4 hits.
    EXPECT(ctrl->num_hits == 4u);

    // Odd tokens are misses: {1,3,5,17,19,33} -> 6 miss tokens (3 groups).
    EXPECT(ctrl->num_miss_tokens == 6u);
    EXPECT(ctrl->total_tokens == 10u);

    // hbm area must hold total_tokens * token_size bytes safely.
    EXPECT(static_cast<std::size_t>(ctrl->total_tokens) * ctrl->token_size
           <= region.hbm_capacity());
}

// ---------------------------------------------------------------------------
// 3) run_three_round_query rejects oversized requests
// ---------------------------------------------------------------------------

void test_run_three_round_query_overflow() {
    SharedRegion region;
    auto* ctrl = region.ctrl();

    // Pretend we have a tiny HBM. Even one hit (576B) won't fit.
    kv_transfer::Key k;
    k.reqid    = "tiny";
    k.layerid  = 0;
    k.tokenids = {0, 2, 4};
    EXPECT(fill_key_device(ctrl->key, k));

    EXPECT(!run_three_round_query(ctrl, region.hbm(), 16));
    EXPECT(ctrl->status == kStatusHbmOverflow);
}

// ---------------------------------------------------------------------------
// 4) Hand-shake: full end-to-end with the simulated kernel.
// ---------------------------------------------------------------------------

void test_handshake_end_to_end() {
    SharedRegion region;
    auto* ctrl = region.ctrl();

    kv_transfer::Key k;
    make_default_key(k);
    EXPECT(fill_key_device(ctrl->key, k));
    ctrl->flag = kFlagIdle;

    // Stamp the HBM area so we can verify the simulator actually reads from it.
    std::memset(region.hbm(), 0xCC, region.hbm_capacity());

    // "Kernel" runs in another thread and uses the same shared region.
    SimResult sim_result;
    std::thread kernel_thread([&]() {
        sim_result = simulate_npu_kernel(
            ctrl, region.hbm(), std::chrono::milliseconds(5000));
    });

    // Host services the request from the main thread.
    const bool ok = service_one_request(
        ctrl, region.hbm(), region.hbm_capacity(),
        std::chrono::milliseconds(5000));

    kernel_thread.join();

    EXPECT(ok);
    EXPECT(sim_result.finished);
    EXPECT(sim_result.observed_response);
    EXPECT(sim_result.status_seen == kStatusOk);
    EXPECT(sim_result.total_tokens_seen == ctrl->total_tokens);
    EXPECT(sim_result.token_size_seen == ctrl->token_size);
    EXPECT(sim_result.first_bytes.size() == ctrl->total_tokens);

    // Final flag state must be kFlagDone (set by the kernel).
    EXPECT(ctrl->flag == kFlagDone);
}

// ---------------------------------------------------------------------------
// 5) Hand-shake: host reports error on overflow, kernel observes kFlagError.
// ---------------------------------------------------------------------------

void test_handshake_propagates_error() {
    // Allocate a region whose HBM area is too small.
    struct TinyRegion {
        alignas(64) unsigned char bytes[sizeof(ControlBlock) + 16] = {};
        ControlBlock* ctrl() { return reinterpret_cast<ControlBlock*>(bytes); }
        void* hbm() { return bytes + sizeof(ControlBlock); }
        std::size_t hbm_capacity() const { return 16; }
    } region;

    auto* ctrl = region.ctrl();
    kv_transfer::Key k;
    k.reqid    = "tiny";
    k.layerid  = 0;
    k.tokenids = {0, 2, 4};
    EXPECT(fill_key_device(ctrl->key, k));
    ctrl->flag = kFlagIdle;

    SimResult sim_result;
    std::thread kernel_thread([&]() {
        sim_result = simulate_npu_kernel(
            ctrl, region.hbm(), std::chrono::milliseconds(5000));
    });

    const bool ok = service_one_request(
        ctrl, region.hbm(), region.hbm_capacity(),
        std::chrono::milliseconds(5000));

    kernel_thread.join();

    EXPECT(!ok);
    EXPECT(sim_result.observed_response);
    // The kernel still raises kFlagDone (it acknowledges the failure).
    EXPECT(sim_result.finished);
    EXPECT(sim_result.status_seen != kStatusOk);
    EXPECT(ctrl->status == kStatusHbmOverflow);
}

// ---------------------------------------------------------------------------
// 6) Hand-shake: host times out cleanly when no kernel ever fires.
// ---------------------------------------------------------------------------

void test_host_times_out_without_kernel() {
    SharedRegion region;
    auto* ctrl = region.ctrl();

    kv_transfer::Key k;
    make_default_key(k);
    EXPECT(fill_key_device(ctrl->key, k));
    ctrl->flag = kFlagIdle;

    // No kernel running -- service_one_request must time out, not hang.
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = service_one_request(
        ctrl, region.hbm(), region.hbm_capacity(),
        std::chrono::milliseconds(50));
    const auto dt = std::chrono::steady_clock::now() - t0;

    EXPECT(!ok);
    EXPECT(std::chrono::duration_cast<std::chrono::milliseconds>(dt).count()
           < 1000);
}

// ---------------------------------------------------------------------------
// 7) Hand-shake content check: the bytes the kernel reads back match what
//    the metadata stubs produced.
// ---------------------------------------------------------------------------

void test_handshake_content_matches_three_round_output() {
    // Build expectation by running run_three_round_query() against a separate
    // shared region, then stamp every hit DRAM page with a unique byte. After
    // a fresh second run, the kernel's first-byte sample should match.
    SharedRegion region;
    auto* ctrl = region.ctrl();

    kv_transfer::Key k;
    k.reqid    = "req-content";
    k.layerid  = 1;
    k.tokenids = {0, 2};                   // both hits, deterministic
    EXPECT(fill_key_device(ctrl->key, k));
    ctrl->flag = kFlagIdle;

    // Stamp the host-side DRAM by running the same path once and writing into
    // the HBM area via process_hits indirectly: easiest is to call the client
    // directly and write into the DRAM source pages.
    kv_transfer::Client client;
    auto r1 = client.first_query(read_key_device(ctrl->key));
    EXPECT(r1.hits.size() == 2u);
    std::memset(r1.hits[0].dram_addr, 0xAA, kv_transfer::kTokenSize);
    std::memset(r1.hits[1].dram_addr, 0xBB, kv_transfer::kTokenSize);

    // Now drive the full hand-shake.
    SimResult sim_result;
    std::thread kernel_thread([&]() {
        sim_result = simulate_npu_kernel(
            ctrl, region.hbm(), std::chrono::milliseconds(5000));
    });
    EXPECT(service_one_request(ctrl, region.hbm(), region.hbm_capacity(),
                               std::chrono::milliseconds(5000)));
    kernel_thread.join();

    EXPECT(ctrl->num_hits == 2u);
    EXPECT(sim_result.first_bytes.size() == ctrl->total_tokens);
    EXPECT(sim_result.first_bytes[0] == 0xAAu);
    EXPECT(sim_result.first_bytes[1] == 0xBBu);
}

}  // namespace

int main() {
    RUN(test_fill_and_read_key_device);
    RUN(test_fill_key_device_rejects_oversized);
    RUN(test_run_three_round_query_layout);
    RUN(test_run_three_round_query_overflow);
    RUN(test_handshake_end_to_end);
    RUN(test_handshake_propagates_error);
    RUN(test_host_times_out_without_kernel);
    RUN(test_handshake_content_matches_three_round_output);
    std::cout << "\nAll npu_bridge tests passed (token=" << kv_transfer::kTokenSize
              << "B, gm=" << kSharedGmBytes << "B).\n";
    return 0;
}
