#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "kvoF_transport.h"

using namespace mooncake::minimal;

// ── helpers ───────────────────────────────────────────────────────────────────

static TransferStatusEnum waitPoll(KVoFTransport& t, BatchID bid,
                                    int timeout_ms = 5000) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    TransferStatus st;
    while (std::chrono::steady_clock::now() < deadline) {
        t.pollBatch(bid, st);
        if (st.status != TransferStatusEnum::PENDING) return st.status;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return TransferStatusEnum::FAILED;
}

// ── MemoryPool ────────────────────────────────────────────────────────────────

TEST(MemoryPool, AcquireRelease) {
    constexpr size_t kSlotSize = 4096;
    constexpr size_t kSlots    = 4;

    void* mem = nullptr;
    ASSERT_EQ(posix_memalign(&mem, 64, kSlotSize * kSlots), 0);
    MemRegion region{mem, kSlotSize * kSlots, 0, 0};

    MemoryPool pool;
    ASSERT_TRUE(pool.init(region, kSlotSize).ok());
    EXPECT_EQ(pool.capacity(), kSlots);
    EXPECT_EQ(pool.slot_size(), kSlotSize);

    std::vector<void*> acquired;
    for (size_t i = 0; i < kSlots; ++i) {
        MemRegion slot;
        ASSERT_TRUE(pool.acquire(slot).ok()) << "slot " << i;
        ASSERT_NE(slot.addr, nullptr);
        EXPECT_GE(slot.addr, mem);
        EXPECT_LT(slot.addr, static_cast<char*>(mem) + kSlotSize * kSlots);
        EXPECT_EQ(slot.length, kSlotSize);
        acquired.push_back(slot.addr);
    }

    MemRegion extra;
    EXPECT_FALSE(pool.acquire(extra).ok());

    pool.release(acquired[0]);
    MemRegion reacquired;
    ASSERT_TRUE(pool.acquire(reacquired).ok());
    EXPECT_EQ(reacquired.addr, acquired[0]);

    std::free(mem);
}

TEST(MemoryPool, SlotDataIsIsolated) {
    constexpr size_t kSlotSize = 256;
    constexpr size_t kSlots    = 2;

    void* mem = nullptr;
    ASSERT_EQ(posix_memalign(&mem, 64, kSlotSize * kSlots), 0);
    std::memset(mem, 0, kSlotSize * kSlots);

    MemRegion region{mem, kSlotSize * kSlots, 0, 0};
    MemoryPool pool;
    ASSERT_TRUE(pool.init(region, kSlotSize).ok());

    MemRegion a, b;
    ASSERT_TRUE(pool.acquire(a).ok());
    ASSERT_TRUE(pool.acquire(b).ok());

    std::memset(a.addr, 0xAA, kSlotSize);
    std::memset(b.addr, 0xBB, kSlotSize);

    auto* pa = static_cast<uint8_t*>(a.addr);
    auto* pb = static_cast<uint8_t*>(b.addr);
    for (size_t i = 0; i < kSlotSize; ++i) {
        EXPECT_EQ(pa[i], 0xAA);
        EXPECT_EQ(pb[i], 0xBB);
    }

    std::free(mem);
}

// ── TCP GET ───────────────────────────────────────────────────────────────────

TEST(TcpKVoF, GetTransfer) {
    constexpr uint16_t kSrvCtrl  = 19900;
    constexpr size_t   kSlotSize = 4096;
    constexpr uint64_t kKey      = 42;

    KVoFTransport server;
    ASSERT_TRUE(server.init("127.0.0.1", kSrvCtrl, "tcp").ok());

    MemRegion srv_region;
    ASSERT_TRUE(server.allocateCache(kSlotSize * 8, kSlotSize, srv_region).ok());

    MemRegion srv_slot;
    ASSERT_TRUE(server.acquireSlot(srv_slot).ok());
    std::memset(srv_slot.addr, 0xAB, kSlotSize);

    server.setMetaSearch([&](uint64_t key) -> MemRegion {
        return (key == kKey) ? srv_slot : MemRegion{};
    });

    KVoFTransport client;
    ASSERT_TRUE(client.init("127.0.0.1", kSrvCtrl + 2, "tcp").ok());

    MemRegion cli_region;
    ASSERT_TRUE(client.allocateCache(kSlotSize * 8, kSlotSize, cli_region).ok());

    MemRegion cli_slot;
    ASSERT_TRUE(client.acquireSlot(cli_slot).ok());
    std::memset(cli_slot.addr, 0x00, kSlotSize);

    RequestEntry e{};
    e.opcode      = OpCode::GET;
    e.key         = kKey;
    e.client_addr = reinterpret_cast<uint64_t>(cli_slot.addr);
    e.client_rkey = 0;
    e.size        = static_cast<uint32_t>(kSlotSize);

    BatchID bid = client.submitAsync("127.0.0.1", kSrvCtrl, {e});
    ASSERT_NE(bid, kInvalidBatchID);
    EXPECT_EQ(waitPoll(client, bid), TransferStatusEnum::COMPLETED);

    auto* recv = static_cast<uint8_t*>(cli_slot.addr);
    for (size_t i = 0; i < kSlotSize; ++i)
        ASSERT_EQ(recv[i], 0xAB) << "mismatch at byte " << i;

    TransferStatus ts;
    ASSERT_TRUE(client.pollBatch(bid, ts).ok());
    EXPECT_EQ(ts.transferred_bytes, kSlotSize);

    client.releaseSlot(cli_slot.addr);
    client.freeBatch(bid);
    server.releaseSlot(srv_slot.addr);
}

// ── TCP PUT ───────────────────────────────────────────────────────────────────

TEST(TcpKVoF, PutTransfer) {
    constexpr uint16_t kSrvCtrl  = 19910;
    constexpr size_t   kSlotSize = 4096;
    constexpr uint64_t kKey      = 99;

    KVoFTransport server;
    ASSERT_TRUE(server.init("127.0.0.1", kSrvCtrl, "tcp").ok());

    MemRegion srv_region;
    ASSERT_TRUE(server.allocateCache(kSlotSize * 8, kSlotSize, srv_region).ok());

    MemRegion srv_slot;
    ASSERT_TRUE(server.acquireSlot(srv_slot).ok());
    std::memset(srv_slot.addr, 0x00, kSlotSize);

    server.setMetaSearch([&](uint64_t key) -> MemRegion {
        return (key == kKey) ? srv_slot : MemRegion{};
    });

    KVoFTransport client;
    ASSERT_TRUE(client.init("127.0.0.1", kSrvCtrl + 2, "tcp").ok());

    MemRegion cli_region;
    ASSERT_TRUE(client.allocateCache(kSlotSize * 8, kSlotSize, cli_region).ok());

    MemRegion cli_slot;
    ASSERT_TRUE(client.acquireSlot(cli_slot).ok());
    std::memset(cli_slot.addr, 0xCD, kSlotSize);

    RequestEntry e{};
    e.opcode      = OpCode::PUT;
    e.key         = kKey;
    e.client_addr = reinterpret_cast<uint64_t>(cli_slot.addr);
    e.client_rkey = 0;
    e.size        = static_cast<uint32_t>(kSlotSize);

    BatchID bid = client.submitAsync("127.0.0.1", kSrvCtrl, {e});
    ASSERT_NE(bid, kInvalidBatchID);
    EXPECT_EQ(waitPoll(client, bid), TransferStatusEnum::COMPLETED);

    auto* dst = static_cast<uint8_t*>(srv_slot.addr);
    for (size_t i = 0; i < kSlotSize; ++i)
        ASSERT_EQ(dst[i], 0xCD) << "mismatch at byte " << i;

    client.releaseSlot(cli_slot.addr);
    client.freeBatch(bid);
    server.releaseSlot(srv_slot.addr);
}

// ── MetaSearch miss → FAILED ──────────────────────────────────────────────────

TEST(TcpKVoF, MetaSearchMiss) {
    constexpr uint16_t kSrvCtrl  = 19920;
    constexpr size_t   kSlotSize = 4096;

    KVoFTransport server;
    ASSERT_TRUE(server.init("127.0.0.1", kSrvCtrl, "tcp").ok());

    MemRegion srv_region;
    ASSERT_TRUE(server.allocateCache(kSlotSize * 4, kSlotSize, srv_region).ok());

    server.setMetaSearch([](uint64_t) -> MemRegion { return {}; });

    KVoFTransport client;
    ASSERT_TRUE(client.init("127.0.0.1", kSrvCtrl + 2, "tcp").ok());

    MemRegion cli_region;
    ASSERT_TRUE(client.allocateCache(kSlotSize * 4, kSlotSize, cli_region).ok());

    MemRegion cli_slot;
    ASSERT_TRUE(client.acquireSlot(cli_slot).ok());

    RequestEntry e{};
    e.opcode      = OpCode::GET;
    e.key         = 9999;
    e.client_addr = reinterpret_cast<uint64_t>(cli_slot.addr);
    e.size        = static_cast<uint32_t>(kSlotSize);

    BatchID bid = client.submitAsync("127.0.0.1", kSrvCtrl, {e});
    ASSERT_NE(bid, kInvalidBatchID);
    EXPECT_EQ(waitPoll(client, bid), TransferStatusEnum::FAILED);

    client.releaseSlot(cli_slot.addr);
    client.freeBatch(bid);
}

// ── Concurrent batches ────────────────────────────────────────────────────────

TEST(TcpKVoF, ConcurrentBatches) {
    constexpr uint16_t kSrvCtrl  = 19930;
    constexpr size_t   kSlotSize = 4096;
    constexpr int      kBatches  = 4;

    KVoFTransport server;
    ASSERT_TRUE(server.init("127.0.0.1", kSrvCtrl, "tcp").ok());

    MemRegion srv_region;
    ASSERT_TRUE(
        server.allocateCache(kSlotSize * 16, kSlotSize, srv_region).ok());

    std::vector<MemRegion> srv_slots(kBatches);
    for (int i = 0; i < kBatches; ++i) {
        ASSERT_TRUE(server.acquireSlot(srv_slots[i]).ok());
        std::memset(srv_slots[i].addr, static_cast<uint8_t>(i + 1), kSlotSize);
    }

    server.setMetaSearch([&](uint64_t key) -> MemRegion {
        if (key < static_cast<uint64_t>(kBatches)) return srv_slots[key];
        return {};
    });

    KVoFTransport client;
    ASSERT_TRUE(client.init("127.0.0.1", kSrvCtrl + 2, "tcp").ok());

    MemRegion cli_region;
    ASSERT_TRUE(
        client.allocateCache(kSlotSize * 16, kSlotSize, cli_region).ok());

    std::vector<BatchID>   bids;
    std::vector<MemRegion> cli_slots(kBatches);
    for (int i = 0; i < kBatches; ++i) {
        ASSERT_TRUE(client.acquireSlot(cli_slots[i]).ok());
        std::memset(cli_slots[i].addr, 0, kSlotSize);

        RequestEntry e{};
        e.opcode      = OpCode::GET;
        e.key         = static_cast<uint64_t>(i);
        e.client_addr = reinterpret_cast<uint64_t>(cli_slots[i].addr);
        e.size        = static_cast<uint32_t>(kSlotSize);

        BatchID bid = client.submitAsync("127.0.0.1", kSrvCtrl, {e});
        ASSERT_NE(bid, kInvalidBatchID);
        bids.push_back(bid);
    }

    for (int i = 0; i < kBatches; ++i) {
        EXPECT_EQ(waitPoll(client, bids[i]), TransferStatusEnum::COMPLETED)
            << "batch " << i;
    }

    for (int i = 0; i < kBatches; ++i) {
        auto* buf = static_cast<uint8_t*>(cli_slots[i].addr);
        for (size_t b = 0; b < kSlotSize; ++b)
            ASSERT_EQ(buf[b], static_cast<uint8_t>(i + 1))
                << "batch " << i << " byte " << b;
        client.releaseSlot(cli_slots[i].addr);
        client.freeBatch(bids[i]);
        server.releaseSlot(srv_slots[i].addr);
    }
}
