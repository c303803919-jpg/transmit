// kv_transfer_ut.cpp
// Lightweight unit tests for the three-round KV transfer skeleton.
// "HBM" is simulated by another plain memory buffer.

#include "kv_transfer.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

using namespace kv_transfer;

namespace {

// Simulated HBM buffer for tests (plain RAM).
alignas(64) char g_sim_hbm[64 * 1024];

#define EXPECT(cond)                                                               \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "FAIL: %s @ %s:%d\n", #cond, __FILE__, __LINE__); \
            std::exit(1);                                                          \
        }                                                                          \
    } while (0)

#define RUN(test_fn)                                  \
    do {                                              \
        std::cout << "[ RUN  ] " #test_fn << '\n';    \
        test_fn();                                    \
        std::cout << "[  OK  ] " #test_fn << '\n';    \
    } while (0)

// ----------------------------------------------------------------------------
// 1) low-level transfer primitives
// ----------------------------------------------------------------------------

void test_basic_dram_to_hbm() {
    char src[kTokenSize];
    char dst[kTokenSize];
    for (std::size_t i = 0; i < kTokenSize; ++i) src[i] = static_cast<char>(i & 0xFF);
    std::memset(dst, 0, sizeof(dst));

    EXPECT(TransferEngine::transfer_dram_to_hbm(src, dst, kTokenSize));
    EXPECT(std::memcmp(src, dst, kTokenSize) == 0);

    // null/zero guards
    EXPECT(!TransferEngine::transfer_dram_to_hbm(nullptr, dst, kTokenSize));
    EXPECT(!TransferEngine::transfer_dram_to_hbm(src, nullptr, kTokenSize));
    EXPECT(!TransferEngine::transfer_dram_to_hbm(src, dst, 0));
}

void test_gather_transfer() {
    char a[kTokenSize], b[kTokenSize], c[kTokenSize];
    std::memset(a, 0xA1, kTokenSize);
    std::memset(b, 0xB2, kTokenSize);
    std::memset(c, 0xC3, kTokenSize);

    char dst[3 * kTokenSize] = {};
    std::vector<const void*> srcs = {a, b, c};
    EXPECT(TransferEngine::gather_transfer(srcs, dst, kTokenSize));
    EXPECT(std::memcmp(dst + 0 * kTokenSize, a, kTokenSize) == 0);
    EXPECT(std::memcmp(dst + 1 * kTokenSize, b, kTokenSize) == 0);
    EXPECT(std::memcmp(dst + 2 * kTokenSize, c, kTokenSize) == 0);

    // null source must fail
    std::vector<const void*> bad = {a, nullptr, c};
    EXPECT(!TransferEngine::gather_transfer(bad, dst, kTokenSize));
}

// ----------------------------------------------------------------------------
// 2) Client round-1: hit/miss split + miss grouping
// ----------------------------------------------------------------------------

void test_first_query_grouping() {
    Client client;
    Key k;
    k.reqid    = "req-001";
    k.layerid  = 7;
    // even -> hit, odd -> miss
    // misses: 1, 3, 5, 17, 19, 33  -> three groups
    //   group0: base=1, members {1,3,5},  offsets {0,2,4},  bitmap 0b00010101 = 0x15
    //   group1: base=17, members {17,19}, offsets {0,2},    bitmap 0b00000101 = 0x05
    //   group2: base=33, members {33},    offsets {0},      bitmap 0x01
    k.tokenids = {0, 1, 2, 3, 4, 5, 8, 17, 19, 33};

    auto r = client.first_query(k);

    // hits: 0,2,4,8 -> 4 entries
    EXPECT(r.hits.size() == 4u);
    EXPECT(r.hits[0].tokenid == 0u);
    EXPECT(r.hits[1].tokenid == 2u);
    EXPECT(r.hits[2].tokenid == 4u);
    EXPECT(r.hits[3].tokenid == 8u);
    for (const auto& h : r.hits) EXPECT(h.dram_addr != nullptr);

    // miss groups
    EXPECT(r.miss_groups.size() == 3u);

    EXPECT(r.miss_groups[0].first_tokenid == 1u);
    EXPECT(r.miss_groups[0].offsets == (std::vector<std::uint32_t>{0, 2, 4}));
    EXPECT(r.miss_groups[0].bitmap == 0x15);

    EXPECT(r.miss_groups[1].first_tokenid == 17u);
    EXPECT(r.miss_groups[1].offsets == (std::vector<std::uint32_t>{0, 2}));
    EXPECT(r.miss_groups[1].bitmap == 0x05);

    EXPECT(r.miss_groups[2].first_tokenid == 33u);
    EXPECT(r.miss_groups[2].offsets == (std::vector<std::uint32_t>{0}));
    EXPECT(r.miss_groups[2].bitmap == 0x01);

    // aggregated key matches first_tokenids
    EXPECT(r.aggregated_key.reqid == "req-001");
    EXPECT(r.aggregated_key.layerid == 7u);
    EXPECT(r.aggregated_key.tokenids_unmap
           == (std::vector<std::uint32_t>{1, 17, 33}));
}

// ----------------------------------------------------------------------------
// 3) Client: DRAM -> "HBM" transfer for hits, with content verification.
// ----------------------------------------------------------------------------

void test_process_hits_transfers_data() {
    Client client;
    Key k;
    k.reqid    = "req-hits";
    k.layerid  = 0;
    k.tokenids = {0, 2, 4, 6};   // all hits in stub
    auto r = client.first_query(k);
    EXPECT(r.hits.size() == 4u);

    // Stamp each hit's DRAM token so we can verify the post-transfer layout.
    for (std::size_t i = 0; i < r.hits.size(); ++i) {
        std::memset(r.hits[i].dram_addr,
                    static_cast<int>(0x10 + i),
                    kTokenSize);
    }

    std::memset(g_sim_hbm, 0, sizeof(g_sim_hbm));
    EXPECT(client.process_hits(r.hits, g_sim_hbm));

    // HBM should now contain the four tokens back-to-back.
    for (std::size_t i = 0; i < r.hits.size(); ++i) {
        char expected[kTokenSize];
        std::memset(expected, static_cast<int>(0x10 + i), kTokenSize);
        EXPECT(std::memcmp(g_sim_hbm + i * kTokenSize, expected, kTokenSize) == 0);
    }

    // null buffer rejected
    EXPECT(!client.process_hits(r.hits, nullptr));
}

// ----------------------------------------------------------------------------
// 4) Client round-2: hash + ip dispatch
// ----------------------------------------------------------------------------

void test_second_query_local_ip() {
    Client client;
    Key1 k1;
    k1.reqid           = "req-002";
    k1.layerid         = 3;
    k1.tokenids_unmap  = {1, 17, 33};

    auto entries = client.second_query(k1);
    EXPECT(entries.size() == 3u);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        EXPECT(entries[i].first_tokenid == k1.tokenids_unmap[i]);
        EXPECT(Client::is_local_ip(entries[i].ip_address));
    }
    // Hash determinism: same key -> same hash.
    auto entries2 = client.second_query(k1);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        EXPECT(entries[i].hash_key == entries2[i].hash_key);
    }
}

void test_remote_transfer_is_stub() {
    Client client;
    SecondQueryEntry e;
    e.ip_address = "10.0.0.42";
    e.hash_key   = 0xdeadbeef;
    EXPECT(!Client::is_local_ip(e.ip_address));
    EXPECT(!client.remote_transfer(e, g_sim_hbm));   // reserved -> false
}

// ----------------------------------------------------------------------------
// 5) Server round-3 + gather DRAM->HBM
// ----------------------------------------------------------------------------

void test_server_gather_transfer() {
    Server server;

    // Two synthetic groups: group0 picks 3 tokens (offsets 0,2,4),
    //                      group1 picks 1 token  (offset 0).
    std::vector<MissGroup> groups(2);
    groups[0].first_tokenid = 1;
    groups[0].bitmap        = 0x15;
    groups[0].offsets       = {0, 2, 4};
    groups[1].first_tokenid = 17;
    groups[1].bitmap        = 0x01;
    groups[1].offsets       = {0};

    std::vector<std::uint64_t> hash_keys = {0x100u, 0x200u};

    // Pre-stamp the server-side DRAM windows so we can verify gather output.
    auto entries = server.third_query(hash_keys);
    EXPECT(entries.size() == 2u);
    for (std::size_t g = 0; g < entries.size(); ++g) {
        char* base = static_cast<char*>(entries[g].dram_addr);
        for (std::size_t slot = 0; slot < kGroupSize; ++slot) {
            // unique byte per (group, slot) so we can detect any mis-gather
            std::memset(base + slot * kTokenSize,
                        static_cast<int>(0x40 + g * 16 + slot),
                        kTokenSize);
        }
    }

    std::memset(g_sim_hbm, 0, sizeof(g_sim_hbm));
    EXPECT(server.handle_transfer_request(hash_keys, groups, g_sim_hbm));

    // Verify gather output layout: group0[0,2,4] then group1[0]
    auto check_token = [](const char* p, int expected) {
        for (std::size_t i = 0; i < kTokenSize; ++i) {
            if (static_cast<unsigned char>(p[i]) != static_cast<unsigned char>(expected))
                return false;
        }
        return true;
    };
    EXPECT(check_token(g_sim_hbm + 0 * kTokenSize, 0x40 + 0 * 16 + 0));
    EXPECT(check_token(g_sim_hbm + 1 * kTokenSize, 0x40 + 0 * 16 + 2));
    EXPECT(check_token(g_sim_hbm + 2 * kTokenSize, 0x40 + 0 * 16 + 4));
    EXPECT(check_token(g_sim_hbm + 3 * kTokenSize, 0x40 + 1 * 16 + 0));

    // Mismatched sizes are rejected.
    EXPECT(!server.handle_transfer_request({0x100u}, groups, g_sim_hbm));
    // Null dst rejected.
    EXPECT(!server.handle_transfer_request(hash_keys, groups, nullptr));
}

// ----------------------------------------------------------------------------
// 6) End-to-end: client round-1 -> hit transfer -> client round-2
//    -> dispatch local -> server round-3 -> gather transfer.
// ----------------------------------------------------------------------------

void test_end_to_end_local_path() {
    Client client;
    Server server;

    Key k;
    k.reqid    = "req-e2e";
    k.layerid  = 5;
    k.tokenids = {0, 1, 2, 3, 4, 5, 17, 18, 19};

    // Round 1
    auto r1 = client.first_query(k);
    EXPECT(!r1.hits.empty());
    EXPECT(!r1.miss_groups.empty());

    // Hit transfer (DRAM -> HBM)
    std::memset(g_sim_hbm, 0, sizeof(g_sim_hbm));
    EXPECT(client.process_hits(r1.hits, g_sim_hbm));

    // Round 2
    auto r2 = client.second_query(r1.aggregated_key);
    EXPECT(r2.size() == r1.miss_groups.size());

    // Dispatch: keep only local-ip entries (in this stub: all of them).
    std::vector<std::uint64_t> local_hashes;
    std::vector<MissGroup>     local_groups;
    for (std::size_t i = 0; i < r2.size(); ++i) {
        if (Client::is_local_ip(r2[i].ip_address)) {
            local_hashes.push_back(r2[i].hash_key);
            local_groups.push_back(r1.miss_groups[i]);
        } else {
            // Reserved path: would call client.remote_transfer(...)
        }
    }
    EXPECT(local_hashes.size() == r1.miss_groups.size());

    // Round 3 + gather, into a region of HBM past the hit area.
    char* miss_dst = g_sim_hbm + r1.hits.size() * kTokenSize;
    EXPECT(server.handle_transfer_request(local_hashes, local_groups, miss_dst));
}

}  // namespace

int main() {
    RUN(test_basic_dram_to_hbm);
    RUN(test_gather_transfer);
    RUN(test_first_query_grouping);
    RUN(test_process_hits_transfers_data);
    RUN(test_second_query_local_ip);
    RUN(test_remote_transfer_is_stub);
    RUN(test_server_gather_transfer);
    RUN(test_end_to_end_local_path);
    std::cout << "\nAll tests passed (token size = " << kTokenSize << "B).\n";
    return 0;
}
