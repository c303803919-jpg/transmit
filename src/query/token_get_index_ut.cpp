// token_get_index_ut.cpp
// Unit tests for kv_transfer::token_get_index. Style matches kv_transfer_ut.cpp:
// no external test framework, EXPECT/RUN macros, single-process binary.

#include "kv_transfer.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace kv_transfer;

namespace {

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::fprintf(stderr, "FAIL: %s @ %s:%d\n",                        \
                         #cond, __FILE__, __LINE__);                          \
            std::exit(1);                                                     \
        }                                                                    \
    } while (0)

#define RUN(test_fn)                                       \
    do {                                                   \
        std::cout << "[ RUN  ] " #test_fn << '\n';         \
        test_fn();                                         \
        std::cout << "[  OK  ] " #test_fn << '\n';         \
    } while (0)

Key make_key() {
    Key k;
    k.reqid    = "req-tgi";
    k.layerid  = 3;
    k.tokenids = {0, 1, 2, 3, 4, 5, 6, 7};
    return k;
}

// ---------------------------------------------------------------------------
// 1) All-VA: only round 1 runs; va_to_hbm called once per token.
// ---------------------------------------------------------------------------
void test_all_va_path() {
    Engine engine;
    engine.set_query_stub(Engine::QueryStubMode::kAlwaysVa);
    engine.reset_counters();

    Key k = make_key();
    EXPECT(token_get_index(k, engine));

    EXPECT(engine.batch_query_count()       == 1u);
    EXPECT(engine.batch_query_local_count() == 0u);
    EXPECT(engine.va_to_hbm_count()         == k.tokenids.size());
    EXPECT(engine.lba_to_hbm_count()        == 0u);
}

// ---------------------------------------------------------------------------
// 2) All-IP: round 1 emits IP+tokenkey for every token; round 2 fires once
//    with the full deferred set; batchQueryLocal returns kVa, so all
//    transfers go through va_to_hbm.
// ---------------------------------------------------------------------------
void test_all_ip_path() {
    Engine engine;
    engine.set_query_stub(Engine::QueryStubMode::kAlwaysIp);
    engine.reset_counters();

    Key k = make_key();
    EXPECT(token_get_index(k, engine));

    EXPECT(engine.batch_query_count()       == 1u);
    EXPECT(engine.batch_query_local_count() == 1u);
    EXPECT(engine.va_to_hbm_count()         == k.tokenids.size());
    EXPECT(engine.lba_to_hbm_count()        == 0u);
}

// ---------------------------------------------------------------------------
// 3) Alternating: half VA, half IP. Round 2 fires with N/2 tokenkeys.
//    Total va_to_hbm calls = N (N/2 in round 1 + N/2 in round 2).
// ---------------------------------------------------------------------------
void test_alternating_path() {
    Engine engine;
    engine.set_query_stub(Engine::QueryStubMode::kAlternating);
    engine.reset_counters();

    Key k = make_key();   // 8 tokens
    EXPECT(token_get_index(k, engine));

    EXPECT(engine.batch_query_count()       == 1u);
    EXPECT(engine.batch_query_local_count() == 1u);
    EXPECT(engine.va_to_hbm_count()         == k.tokenids.size());
    EXPECT(engine.lba_to_hbm_count()        == 0u);
}

// ---------------------------------------------------------------------------
// 4) LBA path: stub emits kLba for every token; lba_to_hbm runs N times,
//    no second-round query.
// ---------------------------------------------------------------------------
void test_lba_path() {
    Engine engine;
    engine.set_query_stub(Engine::QueryStubMode::kAlwaysLba);
    engine.reset_counters();

    Key k = make_key();
    EXPECT(token_get_index(k, engine));

    EXPECT(engine.batch_query_count()       == 1u);
    EXPECT(engine.batch_query_local_count() == 0u);
    EXPECT(engine.va_to_hbm_count()         == 0u);
    EXPECT(engine.lba_to_hbm_count()        == k.tokenids.size());
}

// ---------------------------------------------------------------------------
// 5) Random smoke test: deterministic when seeded; total transfers always
//    equal the number of input tokens (every token must produce exactly one
//    va_to_hbm call once round 2 has resolved).
// ---------------------------------------------------------------------------
void test_random_smoke() {
    Engine engine;
    engine.set_seed(42);
    engine.set_query_stub(Engine::QueryStubMode::kRandom);
    engine.reset_counters();

    Key k = make_key();
    EXPECT(token_get_index(k, engine));

    EXPECT(engine.batch_query_count()       == 1u);
    EXPECT(engine.batch_query_local_count() <= 1u);
    EXPECT(engine.va_to_hbm_count()         == k.tokenids.size());
    EXPECT(engine.lba_to_hbm_count()        == 0u);
}

// ---------------------------------------------------------------------------
// 6) Empty key: no entries, no transfers, no second query, returns true.
// ---------------------------------------------------------------------------
void test_empty_key() {
    Engine engine;
    engine.set_query_stub(Engine::QueryStubMode::kAlwaysIp);
    engine.reset_counters();

    Key k;
    k.reqid   = "empty";
    k.layerid = 0;
    EXPECT(token_get_index(k, engine));

    EXPECT(engine.batch_query_count()       == 1u);
    EXPECT(engine.batch_query_local_count() == 0u);
    EXPECT(engine.va_to_hbm_count()         == 0u);
    EXPECT(engine.lba_to_hbm_count()        == 0u);
}

// ---------------------------------------------------------------------------
// 7) Free-function form (the canonical signature requested by callers)
//    succeeds for a typical key.
// ---------------------------------------------------------------------------
void test_free_function() {
    Key k = make_key();
    EXPECT(token_get_index(k));   // uses an internal default Engine
}

// ---------------------------------------------------------------------------
// 8) Large key: batchQuery and batchQueryLocal must each run exactly once.
// ---------------------------------------------------------------------------
void test_large_key_single_query_per_round() {
    Engine engine;
    engine.set_query_stub(Engine::QueryStubMode::kAlternating);
    engine.reset_counters();

    Key k;
    k.reqid   = "big";
    k.layerid = 0;
    for (std::uint32_t i = 0; i < 64; ++i) {
        k.tokenids.push_back(i);
    }

    EXPECT(token_get_index(k, engine));
    EXPECT(engine.batch_query_count()       == 1u);
    EXPECT(engine.batch_query_local_count() == 1u);
    EXPECT(engine.va_to_hbm_count()         == k.tokenids.size());
}

// ---------------------------------------------------------------------------
// 9) batchQuery shape: one MetaInfo per input tokenid, fields populated
//    according to the type discriminator.
// ---------------------------------------------------------------------------
void test_batch_query_shape() {
    Engine engine;
    engine.set_query_stub(Engine::QueryStubMode::kAlternating);

    Key k = make_key();
    auto out = engine.batchQuery(k);
    EXPECT(out.size() == k.tokenids.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        EXPECT(out[i].tokenid == k.tokenids[i]);
        if (i % 2 == 0) {
            EXPECT(out[i].type == MetaInfoType::kVa);
            EXPECT(out[i].va   != nullptr);
            EXPECT(out[i].ip.empty());
        } else {
            EXPECT(out[i].type == MetaInfoType::kIpTokenkey);
            EXPECT(out[i].va   == nullptr);
            EXPECT(!out[i].ip.empty());
            EXPECT(out[i].tokenkey != 0u);
        }
    }
}

// ---------------------------------------------------------------------------
// 10) batchQueryLocal shape: returns one kVa per input tokenkey, with the
//     tokenkey echoed for cross-reference.
// ---------------------------------------------------------------------------
void test_batch_query_local_shape() {
    Engine engine;
    std::vector<std::uint64_t> tokenkeys = {1, 2, 3, 100, 999};
    auto out = engine.batchQueryLocal(tokenkeys);
    EXPECT(out.size() == tokenkeys.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        EXPECT(out[i].type     == MetaInfoType::kVa);
        EXPECT(out[i].tokenkey == tokenkeys[i]);
        EXPECT(out[i].va       != nullptr);
    }
}

// ---------------------------------------------------------------------------
// 11) HBM buffer wiring: when set_hbm() is called, the dispatcher passes
//     non-null per-token slot pointers to va_to_hbm. We can't observe the
//     argument from the stub's empty body, but we can at least verify the
//     run still succeeds and produces the right number of transfers.
// ---------------------------------------------------------------------------
void test_hbm_buffer_wiring() {
    Engine engine;
    engine.set_query_stub(Engine::QueryStubMode::kAlwaysVa);
    engine.set_token_size(64);
    engine.reset_counters();

    std::vector<unsigned char> hbm(64 * 16, 0);
    engine.set_hbm(hbm.data(), hbm.size());

    Key k = make_key();
    EXPECT(token_get_index(k, engine));
    EXPECT(engine.va_to_hbm_count() == k.tokenids.size());
}

}  // namespace

int main() {
    RUN(test_all_va_path);
    RUN(test_all_ip_path);
    RUN(test_alternating_path);
    RUN(test_lba_path);
    RUN(test_random_smoke);
    RUN(test_empty_key);
    RUN(test_free_function);
    RUN(test_large_key_single_query_per_round);
    RUN(test_batch_query_shape);
    RUN(test_batch_query_local_shape);
    RUN(test_hbm_buffer_wiring);
    std::cout << "\nAll token_get_index tests passed.\n";
    return 0;
}
