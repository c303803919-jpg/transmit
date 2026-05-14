#pragma once

#include <atomic>
#include <future>
#include <memory>
#include <thread>
#include <unordered_map>

#include "minimal_transport.h"

namespace mooncake {
namespace minimal {

// Defined in tcp_minimal_transport.cpp; forward-declared here to avoid
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
    // TCP has no hardware resources to set up; install() is a no-op.
    int  install(const TransportConfig& config) override;
    // Stop server threads and close all persistent connections.
    void uninstall() override;

    // ── Persistent client connections ─────────────────────────────────────────
    // Pre-connect to a server so subsequent submitAsync() calls skip TCP handshake.
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
    void runCtrlServer(std::promise<void> ready);
    void runDataServer(std::promise<void> ready);

    void driveTransfers(const std::string& client_ip, uint16_t data_port,
                        const std::vector<ConcreteTransfer>& xfers,
                        uint64_t& transferred_bytes);

    std::string       bind_ip_;
    uint16_t          ctrl_port_{0};
    uint16_t          data_port_{0};
    std::atomic<bool> running_{false};
    std::thread       ctrl_thread_;
    std::thread       data_thread_;

    std::mutex                           regions_mu_;
    std::unordered_map<void*, MemRegion> local_regions_;

    // Persistent client connections keyed by "server_ip:ctrl_port"
    std::mutex                                                        conns_mu_;
    std::unordered_map<std::string, std::shared_ptr<PersistentConn>> conns_;
};

}  // namespace minimal
}  // namespace mooncake
