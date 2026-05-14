// kvof_client.cpp — cross-machine KVoF client test runner.
//
// Usage:
//   kvof_client --server-ip <IP> --server-port <PORT>
//               [--local-ip <IP>] [--local-port <PORT>]
//               [--slot-size <bytes>]
//
// Runs 4 test cases and prints PASS/FAIL with timing.
// Exits 0 if all pass, non-zero otherwise.

#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <glog/logging.h>

#include "kvoF_transport.h"

static constexpr const char* kBuildVersion = __DATE__ " " __TIME__;

using namespace mooncake::minimal;
using Clk = std::chrono::steady_clock;
using Ms  = std::chrono::milliseconds;

// ── Timestamp helper ──────────────────────────────────────────────────────────

static std::string nowStr() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t   = system_clock::to_time_t(now);
    auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

// ── Poll helper ───────────────────────────────────────────────────────────────

static TransferStatusEnum waitPoll(KVoFTransport& t, BatchID bid,
                                    int timeout_ms = 10000) {
    auto deadline = Clk::now() + Ms(timeout_ms);
    TransferStatus st;
    while (Clk::now() < deadline) {
        t.pollBatch(bid, st);
        if (st.status != TransferStatusEnum::PENDING) return st.status;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return TransferStatusEnum::FAILED;
}

// ── Report ────────────────────────────────────────────────────────────────────

static void report(const char* name, bool ok, long ms) {
    std::cout << "[" << (ok ? "PASS" : "FAIL") << "] " << name
              << "  (" << ms << "ms)\n" << std::flush;
}

// ── Test cases ────────────────────────────────────────────────────────────────

// GET key=0: verify 0x01 in every byte.
static bool testGet(KVoFTransport& client,
                    const std::string& srv_ip, uint16_t srv_port,
                    size_t slot_size, long& elapsed_ms) {
    MemRegion slot;
    if (!client.acquireSlot(slot).ok()) return false;
    std::memset(slot.addr, 0x00, slot_size);

    RequestEntry e{};
    e.opcode      = OpCode::GET;
    e.key         = 0;
    e.client_addr = reinterpret_cast<uint64_t>(slot.addr);
    e.size        = static_cast<uint32_t>(slot_size);

    auto t0 = Clk::now();
    BatchID bid = client.submitAsync(srv_ip, srv_port, {e});
    if (bid == kInvalidBatchID) { client.releaseSlot(slot.addr); return false; }

    auto result = waitPoll(client, bid);
    elapsed_ms = std::chrono::duration_cast<Ms>(Clk::now() - t0).count();

    bool ok = (result == TransferStatusEnum::COMPLETED);
    if (ok) {
        auto* buf = static_cast<uint8_t*>(slot.addr);
        for (size_t i = 0; i < slot_size && ok; ++i) {
            if (buf[i] != 0x01) {
                std::cerr << "  byte[" << i << "]=0x" << std::hex
                          << static_cast<int>(buf[i]) << std::dec
                          << " expected=0x01\n";
                ok = false;
            }
        }
    } else {
        std::cerr << "  status=" << static_cast<int>(result) << " (not COMPLETED)\n";
    }

    client.releaseSlot(slot.addr);
    client.freeBatch(bid);
    return ok;
}

// PUT key=1 with 0xEE, then GET key=1; verify 0xEE.
static bool testPutGet(KVoFTransport& client,
                       const std::string& srv_ip, uint16_t srv_port,
                       size_t slot_size, long& elapsed_ms) {
    auto t0 = Clk::now();

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
    if (!ok) {
        elapsed_ms = std::chrono::duration_cast<Ms>(Clk::now() - t0).count();
        std::cerr << "  PUT step failed\n";
        return false;
    }

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

    auto result = waitPoll(client, gbid);
    elapsed_ms = std::chrono::duration_cast<Ms>(Clk::now() - t0).count();
    ok = (result == TransferStatusEnum::COMPLETED);

    if (ok) {
        auto* buf = static_cast<uint8_t*>(get_slot.addr);
        for (size_t i = 0; i < slot_size && ok; ++i) {
            if (buf[i] != 0xEE) {
                std::cerr << "  byte[" << i << "]=0x" << std::hex
                          << static_cast<int>(buf[i]) << std::dec
                          << " expected=0xEE\n";
                ok = false;
            }
        }
    } else {
        std::cerr << "  GET-back status=" << static_cast<int>(result) << "\n";
    }

    client.releaseSlot(get_slot.addr);
    client.freeBatch(gbid);
    return ok;
}

// GET key=9999 → must produce FAILED (MetaSearch miss).
static bool testMetaMiss(KVoFTransport& client,
                         const std::string& srv_ip, uint16_t srv_port,
                         size_t slot_size, long& elapsed_ms) {
    MemRegion slot;
    if (!client.acquireSlot(slot).ok()) return false;

    RequestEntry e{};
    e.opcode      = OpCode::GET;
    e.key         = 9999;
    e.client_addr = reinterpret_cast<uint64_t>(slot.addr);
    e.size        = static_cast<uint32_t>(slot_size);

    auto t0 = Clk::now();
    BatchID bid = client.submitAsync(srv_ip, srv_port, {e});
    if (bid == kInvalidBatchID) { client.releaseSlot(slot.addr); return false; }

    auto result = waitPoll(client, bid);
    elapsed_ms = std::chrono::duration_cast<Ms>(Clk::now() - t0).count();

    bool ok = (result == TransferStatusEnum::FAILED);
    if (!ok)
        std::cerr << "  expected FAILED, got status="
                  << static_cast<int>(result) << "\n";

    client.releaseSlot(slot.addr);
    client.freeBatch(bid);
    return ok;
}

// 4 concurrent GETs for keys 4-7 (untouched by earlier tests).
// Server slot[k] pre-filled with (k+1)&0xFF → keys 4-7 → bytes 5-8.
static bool testConcurrent(KVoFTransport& client,
                            const std::string& srv_ip, uint16_t srv_port,
                            size_t slot_size, long& elapsed_ms) {
    constexpr int      kN    = 4;
    constexpr uint64_t kBase = 4;

    std::vector<MemRegion> slots(kN);
    std::vector<BatchID>   bids(kN, kInvalidBatchID);
    bool setup_ok = true;

    auto t0 = Clk::now();

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
            auto result = waitPoll(client, bids[i]);
            if (result != TransferStatusEnum::COMPLETED) {
                std::cerr << "  batch[" << i << "] key=" << (kBase + i)
                          << " status=" << static_cast<int>(result) << "\n";
                ok = false;
            } else {
                auto* buf = static_cast<uint8_t*>(slots[i].addr);
                uint8_t expected = static_cast<uint8_t>((kBase + i + 1) & 0xFF);
                for (size_t b = 0; b < slot_size && ok; ++b) {
                    if (buf[b] != expected) {
                        std::cerr << "  batch[" << i << "] byte[" << b
                                  << "]=0x" << std::hex
                                  << static_cast<int>(buf[b]) << std::dec
                                  << " expected=0x" << std::hex
                                  << static_cast<int>(expected) << std::dec << "\n";
                        ok = false;
                    }
                }
            }
            client.freeBatch(bids[i]);
        }
        client.releaseSlot(slots[i].addr);
    }

    elapsed_ms = std::chrono::duration_cast<Ms>(Clk::now() - t0).count();
    return ok;
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;
    FLAGS_minloglevel = 0;

    std::string srv_ip     = "127.0.0.1";
    uint16_t    srv_port   = 20000;
    std::string local_ip   = "0.0.0.0";
    uint16_t    local_port = 20010;
    size_t      slot_size  = 4096;

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

    std::cout << "=== kvof_client ===\n"
              << "build:      " << kBuildVersion << "\n"
              << "started:    " << nowStr() << "\n"
              << "server:     " << srv_ip << ":" << srv_port << "\n"
              << "local:      " << local_ip << ":" << local_port << "\n"
              << "slot_size:  " << slot_size << " bytes\n"
              << std::flush;

    KVoFTransport client;
    auto s = client.init(local_ip, local_port, "tcp");
    if (!s.ok()) {
        std::cerr << "[" << nowStr() << "] ERROR client init: " << s.ToString() << "\n";
        return 1;
    }

    MemRegion region;
    s = client.allocateCache(slot_size * 16, slot_size, region);
    if (!s.ok()) {
        std::cerr << "[" << nowStr() << "] ERROR allocateCache: " << s.ToString() << "\n";
        return 1;
    }

    std::cout << "─────────────────────────────────────────────────────\n"
              << std::flush;

    int failed = 0;
    long ms = 0;

    auto run = [&](const char* name, auto fn) {
        std::cout << "[....] " << name << "\n" << std::flush;
        bool ok = fn(ms);
        report(name, ok, ms);
        if (!ok) ++failed;
    };

    run("GET key=0 → verify 0x01",
        [&](long& t) { return testGet(client, srv_ip, srv_port, slot_size, t); });

    run("PUT key=1 (0xEE) then GET → verify 0xEE",
        [&](long& t) { return testPutGet(client, srv_ip, srv_port, slot_size, t); });

    run("GET key=9999 → FAILED (meta miss)",
        [&](long& t) { return testMetaMiss(client, srv_ip, srv_port, slot_size, t); });

    run("4 concurrent GETs keys 4-7 → verify fill bytes",
        [&](long& t) { return testConcurrent(client, srv_ip, srv_port, slot_size, t); });

    std::cout << "─────────────────────────────────────────────────────\n"
              << (4 - failed) << "/4 passed"
              << "  finished: " << nowStr() << "\n";
    return (failed == 0) ? 0 : 1;
}
