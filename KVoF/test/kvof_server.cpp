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
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <glog/logging.h>

#include "kvoF_transport.h"

using namespace mooncake::minimal;

static volatile sig_atomic_t g_stop = 0;
static void sighandler(int) { g_stop = 1; }

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;

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

    KVoFTransport transport;
    auto s = transport.init(ip, port, "tcp");
    if (!s.ok()) {
        std::cerr << "init failed: " << s.ToString() << "\n";
        return 1;
    }

    MemRegion region;
    s = transport.allocateCache(slots * slot_size, slot_size, region);
    if (!s.ok()) {
        std::cerr << "allocateCache failed: " << s.ToString() << "\n";
        return 1;
    }

    std::vector<MemRegion> srv_slots(slots);
    for (size_t i = 0; i < slots; ++i) {
        if (!transport.acquireSlot(srv_slots[i]).ok()) {
            std::cerr << "acquireSlot " << i << " failed\n";
            return 1;
        }
        std::memset(srv_slots[i].addr,
                    static_cast<uint8_t>((i + 1) & 0xFF),
                    slot_size);
    }

    transport.setMetaSearch([&](uint64_t key) -> MemRegion {
        if (key < slots) return srv_slots[key];
        return {};
    });

    std::signal(SIGINT,  sighandler);
    std::signal(SIGTERM, sighandler);

    std::cout << "READY ip=" << ip
              << " ctrl_port=" << port
              << " data_port=" << (port + 1)
              << " slots=" << slots
              << " slot_size=" << slot_size
              << "\n" << std::flush;

    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "Server shutting down\n";
    return 0;
}
