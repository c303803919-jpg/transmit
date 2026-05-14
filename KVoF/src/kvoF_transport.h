#pragma once

#include <memory>
#include <string>
#include <vector>

#include "minimal_transport.h"

namespace mooncake {
namespace minimal {

class KVoFTransport {
public:
    KVoFTransport() = default;
    ~KVoFTransport();

    // Create transport ("tcp" or "rdma"), bind local IP + ctrl port, start server.
    Status init(const std::string& bind_ip, uint16_t ctrl_port,
                const std::string& transport);

    // Transport-specific hardware setup (RDMA device open, PD/CQ creation).
    // Must be called after init().  TCP is a no-op.
    int  install(const TransportConfig& config);

    // Reverse of install: stop threads and release hardware resources.
    void uninstall();

    // Client: pre-connect to a server to avoid per-request TCP handshake.
    // For RDMA this also performs the QP exchange handshake.
    Status connectServer(const std::string& server_ip, uint16_t server_ctrl_port);

    // Release a pre-established connection.
    void disconnectServer(const std::string& server_ip, uint16_t server_ctrl_port);

    Status allocateCache(size_t total_bytes, size_t slot_size,
                         MemRegion& region_out);

    void setMetaSearch(MetaSearch fn);

    Status acquireSlot(MemRegion& out);
    void   releaseSlot(void* addr);

    BatchID submitAsync(const std::string& server_ip, uint16_t server_ctrl_port,
                        const std::vector<RequestEntry>& entries);
    Status  pollBatch(BatchID id, TransferStatus& out);
    Status  freeBatch(BatchID id);

    std::string localEndpoint() const;

private:
    std::unique_ptr<MinimalTransport> transport_;
    std::unique_ptr<MemoryPool>       pool_;
    std::string                        bind_ip_;
    uint16_t                           ctrl_port_{0};
    void*                              cache_mem_{nullptr};
    size_t                             cache_bytes_{0};
};

}  // namespace minimal
}  // namespace mooncake
