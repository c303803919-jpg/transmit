// kvof_client.cpp — cross-machine KVoF client test runner.
//
// Usage:
//   kvof_client --server-ip <IP> --server-port <PORT>
//               [--local-ip <IP>] [--local-port <PORT>]
//               [--slot-size <bytes>]
//
// Runs 4 test cases and prints PASS/FAIL per case.
// Exits 0 if all pass, non-zero otherwise.
//
// Expected server state (kvof_server defaults):
//   slot[i] filled with byte (i+1) & 0xFF, MetaSearch: key k → slot k

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <glog/logging.h>

#include "kvoF_transport.h"

using namespace mooncake::minimal;

// ── Helpers ───────────────────────────────────────────────────────────────────

static TransferStatusEnum waitPoll(KVoFTransport& t, BatchID bid,
                                    int timeout_ms = 10000) {
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

static void report(const char* name, bool ok) {
    std::cout << "[" << (ok ? "PASS" : "FAIL") << "] " << name << "\n"
              << std::flush;
}

// ── Test cases ────────────────────────────────────────────────────────────────

// GET key=0: server slot[0] pre-filled with 0x01; verify client receives 0x01.
static bool testGet(KVoFTransport& client,
                    const std::string& srv_ip, uint16_t srv_port,
                    size_t slot_size) {
    MemRegion slot;
    if (!client.acquireSlot(slot).ok()) return false;
    std::memset(slot.addr, 0x00, slot_size);

    RequestEntry e{};
    e.opcode      = OpCode::GET;
    e.key         = 0;
    e.client_addr = reinterpret_cast<uint64_t>(slot.addr);
    e.size        = static_cast<uint32_t>(slot_size);

    BatchID bid = client.submitAsync(srv_ip, srv_port, {e});
    if (bid == kInvalidBatchID) { client.releaseSlot(slot.addr); return false; }

    bool ok = (waitPoll(client, bid) == TransferStatusEnum::COMPLETED);

    if (ok) {
        auto* buf = static_cast<uint8_t*>(slot.addr);
        for (size_t i = 0; i < slot_size && ok; ++i)
            ok = (buf[i] == 0x01);
    }

    client.releaseSlot(slot.addr);
    client.freeBatch(bid);
    return ok;
}

// PUT key=1 with 0xEE, then GET key=1 back; verify 0xEE.
static bool testPutGet(KVoFTransport& client,
                       const std::string& srv_ip, uint16_t srv_port,
                       size_t slot_size) {
    // PUT
    MemRegion put_slot;
    if (!client.acquireSlot(put_slot).ok()) return false;
    std::memset(put_slot.addr, 0xEE, slot_size);

    RequestEntry pe{};
    pe.opcode      = OpCode::PUT;
    pe.key         = 1;
    pe.client_addr = reinterpret_cast<uint64_t>(put_slot.addr);
    pe.size        = static_cast<uint32_t>(slot_size);

    BatchID pbid = client.submitAsync(srv_ip, srv_port, {pe});
    if (pbid == kInvalidBatchID) { client.releaseSlot(put_slot.addr); return false; }

    bool ok = (waitPoll(client, pbid) == TransferStatusEnum::COMPLETED);
    client.releaseSlot(put_slot.addr);
    client.freeBatch(pbid);
    if (!ok) return false;

    // GET back
    MemRegion get_slot;
    if (!client.acquireSlot(get_slot).ok()) return false;
    std::memset(get_slot.addr, 0x00, slot_size);

    RequestEntry ge{};
    ge.opcode      = OpCode::GET;
    ge.key         = 1;
    ge.client_addr = reinterpret_cast<uint64_t>(get_slot.addr);
    ge.size        = static_cast<uint32_t>(slot_size);

    BatchID gbid = client.submitAsync(srv_ip, srv_port, {ge});
    if (gbid == kInvalidBatchID) { client.releaseSlot(get_slot.addr); return false; }

    ok = (waitPoll(client, gbid) == TransferStatusEnum::COMPLETED);
    if (ok) {
        auto* buf = static_cast<uint8_t*>(get_slot.addr);
        for (size_t i = 0; i < slot_size && ok; ++i)
            ok = (buf[i] == 0xEE);
    }

    client.releaseSlot(get_slot.addr);
    client.freeBatch(gbid);
    return ok;
}

// GET nonexistent key → server MetaSearch returns empty → FAILED status.
static bool testMetaMiss(KVoFTransport& client,
                         const std::string& srv_ip, uint16_t srv_port,
                         size_t slot_size) {
    MemRegion slot;
    if (!client.acquireSlot(slot).ok()) return false;

    RequestEntry e{};
    e.opcode      = OpCode::GET;
    e.key         = 9999;
    e.client_addr = reinterpret_cast<uint64_t>(slot.addr);
    e.size        = static_cast<uint32_t>(slot_size);

    BatchID bid = client.submitAsync(srv_ip, srv_port, {e});
    if (bid == kInvalidBatchID) { client.releaseSlot(slot.addr); return false; }

    bool ok = (waitPoll(client, bid) == TransferStatusEnum::FAILED);

    client.releaseSlot(slot.addr);
    client.freeBatch(bid);
    return ok;
}

// 4 concurrent GETs for keys 4-7 (untouched by earlier tests).
// Server slot[k] is pre-filled with byte (k+1) & 0xFF → keys 4-7 → bytes 5-8.
static bool testConcurrent(KVoFTransport& client,
                            const std::string& srv_ip, uint16_t srv_port,
                            size_t slot_size) {
    constexpr int    kN       = 4;
    constexpr uint64_t kBase  = 4;  // start at key=4 to avoid slots dirtied by PUT test

    std::vector<MemRegion> slots(kN);
    std::vector<BatchID>   bids(kN, kInvalidBatchID);
    bool setup_ok = true;

    for (int i = 0; i < kN && setup_ok; ++i) {
        if (!client.acquireSlot(slots[i]).ok()) { setup_ok = false; break; }
        std::memset(slots[i].addr, 0x00, slot_size);

        RequestEntry e{};
        e.opcode      = OpCode::GET;
        e.key         = kBase + static_cast<uint64_t>(i);
        e.client_addr = reinterpret_cast<uint64_t>(slots[i].addr);
        e.size        = static_cast<uint32_t>(slot_size);

        bids[i] = client.submitAsync(srv_ip, srv_port, {e});
        if (bids[i] == kInvalidBatchID) setup_ok = false;
    }

    bool ok = setup_ok;
    for (int i = 0; i < kN; ++i) {
        if (!slots[i].addr) continue;

        if (bids[i] != kInvalidBatchID) {
            if (waitPoll(client, bids[i]) != TransferStatusEnum::COMPLETED) {
                ok = false;
            } else {
                auto* buf = static_cast<uint8_t*>(slots[i].addr);
                uint8_t expected = static_cast<uint8_t>((kBase + i + 1) & 0xFF);
                for (size_t b = 0; b < slot_size && ok; ++b)
                    ok = (buf[b] == expected);
            }
            client.freeBatch(bids[i]);
        }
        client.releaseSlot(slots[i].addr);
    }
    return ok;
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;

    std::string srv_ip    = "127.0.0.1";
    uint16_t    srv_port  = 20000;
    std::string local_ip  = "0.0.0.0";
    uint16_t    local_port = 20010;
    size_t      slot_size = 4096;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--server-ip"   && i + 1 < argc) srv_ip     = argv[++i];
        else if (a == "--server-port" && i + 1 < argc) srv_port   = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (a == "--local-ip"    && i + 1 < argc) local_ip   = argv[++i];
        else if (a == "--local-port"  && i + 1 < argc) local_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (a == "--slot-size"   && i + 1 < argc) slot_size  = std::stoul(argv[++i]);
        else if (a == "--help") {
            std::cout << "Usage: kvof_client --server-ip IP --server-port PORT\n"
                         "                   [--local-ip IP] [--local-port PORT]\n"
                         "                   [--slot-size BYTES]\n";
            return 0;
        }
    }

    std::cout << "Connecting to server " << srv_ip << ":" << srv_port << "\n";

    KVoFTransport client;
    auto s = client.init(local_ip, local_port, "tcp");
    if (!s.ok()) {
        std::cerr << "client init failed: " << s.ToString() << "\n";
        return 1;
    }

    MemRegion region;
    s = client.allocateCache(slot_size * 16, slot_size, region);
    if (!s.ok()) {
        std::cerr << "allocateCache failed: " << s.ToString() << "\n";
        return 1;
    }

    int failed = 0;

    bool r1 = testGet(client, srv_ip, srv_port, slot_size);
    report("GET key=0 → verify 0x01", r1);
    if (!r1) ++failed;

    bool r2 = testPutGet(client, srv_ip, srv_port, slot_size);
    report("PUT key=1 (0xEE) then GET → verify 0xEE", r2);
    if (!r2) ++failed;

    bool r3 = testMetaMiss(client, srv_ip, srv_port, slot_size);
    report("GET key=9999 → FAILED (meta miss)", r3);
    if (!r3) ++failed;

    bool r4 = testConcurrent(client, srv_ip, srv_port, slot_size);
    report("4 concurrent GETs keys 0-3 → verify fill bytes", r4);
    if (!r4) ++failed;

    std::cout << "\n" << (4 - failed) << "/4 tests passed.\n";
    return (failed == 0) ? 0 : 1;
}
