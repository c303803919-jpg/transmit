#include <cstdlib>
#include <cstring>

#include "kvoF_transport.h"
#include "tcp_minimal_transport.h"

namespace mooncake {
namespace minimal {

// ── MemoryPool ────────────────────────────────────────────────────────────────

Status MemoryPool::init(MemRegion region, size_t slot_size) {
    if (!region.addr || region.length == 0 || slot_size == 0)
        return Status::InvalidArgument("MemoryPool::init: invalid arguments");
    if (region.length < slot_size)
        return Status::InvalidArgument("MemoryPool::init: region smaller than slot");

    base_      = region;
    slot_size_ = slot_size;
    capacity_  = region.length / slot_size;

    auto* base = static_cast<char*>(region.addr);
    for (size_t i = 0; i < capacity_; ++i)
        free_slots_.push(base + i * slot_size);

    return Status::OK();
}

Status MemoryPool::acquire(MemRegion& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (free_slots_.empty())
        return Status::Memory("MemoryPool exhausted");

    void* addr = free_slots_.top();
    free_slots_.pop();
    out = MemRegion{addr, slot_size_, base_.lkey, base_.rkey};
    return Status::OK();
}

void MemoryPool::release(void* addr) {
    std::lock_guard<std::mutex> lock(mu_);
    free_slots_.push(addr);
}

// ── KVoFTransport ─────────────────────────────────────────────────────────────

KVoFTransport::~KVoFTransport() {
    if (cache_mem_) {
        if (transport_) transport_->unregisterMemory(cache_mem_);
        std::free(cache_mem_);
        cache_mem_ = nullptr;
    }
}

Status KVoFTransport::init(const std::string& bind_ip, uint16_t ctrl_port,
                            const std::string& transport) {
    bind_ip_   = bind_ip;
    ctrl_port_ = ctrl_port;

    if (transport == "tcp") {
        transport_ = std::make_unique<TcpMinimalTransport>();
    } else {
        return Status::NotSupportedTransport("unknown transport: " + transport);
    }

    if (transport_->init(bind_ip, ctrl_port) != 0)
        return Status::Context("transport init failed");
    return Status::OK();
}

int KVoFTransport::install(const TransportConfig& config) {
    if (!transport_) return -1;
    return transport_->install(config);
}

void KVoFTransport::uninstall() {
    if (transport_) transport_->uninstall();
}

Status KVoFTransport::connectServer(const std::string& server_ip,
                                     uint16_t server_ctrl_port) {
    if (!transport_) return Status::Context("transport not initialized");
    return transport_->connectServer(server_ip, server_ctrl_port);
}

void KVoFTransport::disconnectServer(const std::string& server_ip,
                                      uint16_t server_ctrl_port) {
    if (transport_) transport_->disconnectServer(server_ip, server_ctrl_port);
}

Status KVoFTransport::allocateCache(size_t total_bytes, size_t slot_size,
                                     MemRegion& region_out) {
    constexpr size_t kAlign = 64;
    if (posix_memalign(&cache_mem_, kAlign, total_bytes) != 0 || !cache_mem_)
        return Status::Memory("failed to allocate cache memory");

    cache_bytes_ = total_bytes;
    std::memset(cache_mem_, 0, total_bytes);

    Status s = transport_->registerMemory(cache_mem_, total_bytes, region_out);
    if (!s.ok()) {
        std::free(cache_mem_);
        cache_mem_ = nullptr;
        return s;
    }

    pool_ = std::make_unique<MemoryPool>();
    s = pool_->init(region_out, slot_size);
    if (!s.ok()) return s;

    return Status::OK();
}

void KVoFTransport::setMetaSearch(MetaSearch fn) {
    transport_->setMetaSearch(std::move(fn));
}

Status KVoFTransport::acquireSlot(MemRegion& out) {
    if (!pool_) return Status::Context("pool not initialized");
    return pool_->acquire(out);
}

void KVoFTransport::releaseSlot(void* addr) {
    if (pool_) pool_->release(addr);
}

BatchID KVoFTransport::submitAsync(const std::string& server_ip,
                                    uint16_t server_ctrl_port,
                                    const std::vector<RequestEntry>& entries) {
    return transport_->submitAsync(server_ip, server_ctrl_port, entries);
}

Status KVoFTransport::pollBatch(BatchID id, TransferStatus& out) {
    return transport_->pollBatch(id, out);
}

Status KVoFTransport::freeBatch(BatchID id) {
    return transport_->freeBatch(id);
}

std::string KVoFTransport::localEndpoint() const {
    return bind_ip_ + ":" + std::to_string(ctrl_port_);
}

}  // namespace minimal
}  // namespace mooncake
