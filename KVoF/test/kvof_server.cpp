// kvof_server.cpp — standalone KVoF server for cross-machine testing.
//
// Usage:
//   kvof_server [--ip <bind_ip>] [--port <ctrl_port>]
//               [--slots <n>] [--slot-size <bytes>]
//
// Slot i is pre-filled with byte value ((i+1) & 0xFF).
// MetaSearch maps key k → slot k (k < slots).
// Prints "READY ..." then blocks until SIGINT/SIGTERM.

#include <chrono>
#include <csignal>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <glog/logging.h>

#include "kvoF_transport.h"

// Compile-time build timestamp used as a simple version ID.
static constexpr const char* kBuildVersion = __DATE__ " " __TIME__;

using namespace mooncake::minimal;

// ── Timestamp helper ──────────────────────────────────────────────────────────

static std::string nowStr() {
    using namespace std::chrono;
    auto now  = system_clock::now();
    auto t    = system_clock::to_time_t(now);
    auto ms   = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

// ── Signal handling ───────────────────────────────────────────────────────────

static volatile sig_atomic_t g_stop = 0;
static void sighandler(int) { g_stop = 1; }

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;
    FLAGS_minloglevel = 0;  // INFO and above

    std::string ip        = "0.0.0.0";
    uint16_t    port      = 20000;
    size_t      slots     = 8;
    size_t      slot_size = 4096;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--ip"        && i + 1 < argc) ip        = argv[++i];
        else if (a == "--port"      && i + 1 < argc) port      = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (a == "--slots"     && i + 1 < argc) slots     = std::stoul(argv[++i]);
        else if (a == "--slot-size" && i + 1 < argc) slot_size = std::stoul(argv[++i]);
        else if (a == "--help") {
            std::cout << "Usage: kvof_server [--ip IP] [--port PORT] "
                         "[--slots N] [--slot-size BYTES]\n";
            return 0;
        }
    }

    std::cout << "=== kvof_server ===\n"
              << "build:     " << kBuildVersion << "\n"
              << "started:   " << nowStr() << "\n"
              << "ip:        " << ip << "\n"
              << "port:      " << port << "\n"
              << "slots:     " << slots << "\n"
              << "slot_size: " << slot_size << " bytes\n"
              << std::flush;

    KVoFTransport transport;
    auto s = transport.init(ip, port, "tcp");
    if (!s.ok()) {
        std::cerr << "[" << nowStr() << "] ERROR init: " << s.ToString() << "\n";
        return 1;
    }

    MemRegion region;
    s = transport.allocateCache(slots * slot_size, slot_size, region);
    if (!s.ok()) {
        std::cerr << "[" << nowStr() << "] ERROR allocateCache: " << s.ToString() << "\n";
        return 1;
    }

    std::vector<MemRegion> srv_slots(slots);
    for (size_t i = 0; i < slots; ++i) {
        if (!transport.acquireSlot(srv_slots[i]).ok()) {
            std::cerr << "[" << nowStr() << "] ERROR acquireSlot " << i << "\n";
            return 1;
        }
        uint8_t fill = static_cast<uint8_t>((i + 1) & 0xFF);
        std::memset(srv_slots[i].addr, fill, slot_size);
        std::cout << "slot[" << i << "] addr=" << srv_slots[i].addr
                  << " fill=0x" << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<int>(fill) << std::dec << "\n";
    }
    std::cout << std::flush;

    transport.setMetaSearch([&](uint64_t key) -> MemRegion {
        if (key < slots) {
            LOG(INFO) << "[server] MetaSearch key=" << key
                      << " → HIT addr=" << srv_slots[key].addr;
            return srv_slots[key];
        }
        LOG(INFO) << "[server] MetaSearch key=" << key << " → MISS";
        return {};
    });

    std::signal(SIGINT,  sighandler);
    std::signal(SIGTERM, sighandler);

    std::cout << "READY ip=" << ip
              << " ctrl_port=" << port
              << " slots=" << slots
              << " slot_size=" << slot_size
              << " build=" << kBuildVersion
              << "\n" << std::flush;

    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "[" << nowStr() << "] Server shutting down\n";
    return 0;
}
