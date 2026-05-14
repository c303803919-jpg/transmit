#pragma once

#include <atomic>
#include <future>
#include <memory>
#include <thread>
#include <unordered_map>

#include "minimal_transport.h"

namespace mooncake {
namespace minimal {

// Defined in tcp_minimal_transport.cpp; forward-declared to avoid
// pulling asio headers into this interface header.
struct PersistentConn;

class TcpMinimalTransport : public MinimalTransport {
public:
    TcpMinimalTransport() = default;
    ~TcpMinimalTransport() override;

    // ── Server / memory API ───────────────────────────────────────────────────
    int    init(const std::string& bind_ip, uint16_t ctrl_port) override;
    Status registerMemory(void* addr, size_t length, MemRegion& out) override;
    void   unregisterMemory(void* addr) override;
    void   setMetaSearch(MetaSearch fn) override;

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    int  install(const TransportConfig& config) override;
    void uninstall() override;

    // ── Persistent client connections ─────────────────────────────────────────
    Status connectServer(const std::string& server_ip,
                         uint16_t server_ctrl_port) override;
    void   disconnectServer(const std::string& server_ip,
                            uint16_t server_ctrl_port) override;

    // ── Transfer API ──────────────────────────────────────────────────────────
    BatchID submitAsync(const std::string& server_ip,
                        uint16_t server_ctrl_port,
                        const std::vector<RequestEntry>& entries) override;

    Status pollBatch(BatchID id, TransferStatus& out) override;
    Status freeBatch(BatchID id) override;

private:
    // Server: accept ctrl connections and handle data inline on the same socket.
    void runCtrlServer(std::promise<void> ready);

    std::string       bind_ip_;
    uint16_t          ctrl_port_{0};
    std::atomic<bool> running_{false};
    std::thread       ctrl_thread_;

    std::mutex                           regions_mu_;
    std::unordered_map<void*, MemRegion> local_regions_;

    std::mutex                                                        conns_mu_;
    std::unordered_map<std::string, std::shared_ptr<PersistentConn>> conns_;
};

}  // namespace minimal
}  // namespace mooncake
