#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stack>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/base/status.h"

namespace mooncake {
namespace minimal {

// ── Types ─────────────────────────────────────────────────────────────────────

using BatchID = uint64_t;
static constexpr BatchID kInvalidBatchID = UINT64_MAX;

enum class TransferStatusEnum : uint8_t { PENDING, COMPLETED, FAILED };

struct TransferStatus {
    TransferStatusEnum status{TransferStatusEnum::PENDING};
    uint64_t           transferred_bytes{0};
};

struct MemRegion {
    void*    addr{nullptr};
    size_t   length{0};
    uint32_t lkey{0};
    uint32_t rkey{0};
};

enum class OpCode : uint8_t { GET = 0, PUT = 1 };

// ── Control wire format (client → server over TCP ctrl socket) ────────────────

static constexpr uint32_t kControlMagic = 0x4B564F46;  // "KVOF"

struct RequestEntry {
    uint64_t key;
    uint64_t client_addr;
    uint32_t client_rkey;
    uint32_t size;
    OpCode   opcode;
    uint8_t  _pad[3]{};
};

struct ControlRequestHeader {
    uint32_t magic;
    uint16_t count;
    uint16_t data_port;
};

struct ControlResponse {
    uint32_t magic;
    int32_t  status;
    uint64_t transferred_bytes;
};

// ── Data wire format ──────────────────────────────────────────────────────────

struct DataSessionHeader {
    uint64_t size;
    uint64_t addr;
    uint8_t  opcode;  // 0=WRITE (push), 1=READ (pull)
};

// ── Server-internal ───────────────────────────────────────────────────────────

struct ConcreteTransfer {
    OpCode   opcode;
    void*    local_addr;
    uint64_t remote_addr;
    size_t   length;
    uint32_t local_lkey{0};
    uint32_t remote_rkey{0};
};

// ── MetaSearch callback ───────────────────────────────────────────────────────

using MetaSearch = std::function<MemRegion(uint64_t key)>;

// ── BatchState (all-atomic, no mutex, no cv) ──────────────────────────────────

struct BatchState {
    std::atomic<TransferStatusEnum> status{TransferStatusEnum::PENDING};
    std::atomic<uint64_t>           transferred_bytes{0};
};

// ── MemoryPool ────────────────────────────────────────────────────────────────

class MemoryPool {
public:
    Status init(MemRegion region, size_t slot_size);
    Status acquire(MemRegion& out);
    void   release(void* addr);

    size_t slot_size() const { return slot_size_; }
    size_t capacity() const { return capacity_; }

private:
    std::mutex        mu_;
    std::stack<void*> free_slots_;
    MemRegion         base_{};
    size_t            slot_size_{0};
    size_t            capacity_{0};
};

// ── Transport configuration (RDMA parameters; ignored for TCP) ─────────────────

struct TransportConfig {
    std::string rdma_device;  // IB device name, e.g. "mlx5_0"; empty = first available
    int         rdma_port{1};
    int         gid_index{1};
};

// ── MinimalTransport pure interface ──────────────────────────────────────────

class MinimalTransport {
public:
    virtual ~MinimalTransport() = default;

    // Bind local IP + ctrl port and start server threads.
    virtual int    init(const std::string& bind_ip, uint16_t ctrl_port) = 0;
    virtual Status registerMemory(void* addr, size_t length, MemRegion& out) = 0;
    virtual void   unregisterMemory(void* addr) = 0;
    virtual void   setMetaSearch(MetaSearch fn) = 0;

    // Transport lifecycle: hardware-specific setup after init().
    // TCP: no-op.  RDMA: open IB device, create PD/CQ.
    virtual int  install(const TransportConfig& config) { return 0; }

    // Reverse of install: stop threads and release hardware resources.
    virtual void uninstall() {}

    // Client: pre-connect to a server so subsequent submitAsync() calls
    // can reuse the connection (avoiding per-request TCP handshake overhead).
    // For RDMA this also performs the QP exchange handshake.
    virtual Status connectServer(const std::string& server_ip,
                                 uint16_t server_ctrl_port) {
        return Status::OK();
    }

    // Release a pre-established connection.
    virtual void disconnectServer(const std::string& server_ip,
                                  uint16_t server_ctrl_port) {}

    virtual BatchID submitAsync(const std::string& server_ip,
                                uint16_t server_ctrl_port,
                                const std::vector<RequestEntry>& entries) = 0;

    virtual Status pollBatch(BatchID id, TransferStatus& out) = 0;
    virtual Status freeBatch(BatchID id) = 0;

protected:
    MetaSearch meta_search_;

    std::mutex batches_mu_;
    std::unordered_map<BatchID, std::shared_ptr<BatchState>> batches_;
};

}  // namespace minimal
}  // namespace mooncake
