#include "tcp_minimal_transport.h"

#include <glog/logging.h>
#include <sys/socket.h>
#include <unistd.h>

#include <asio/connect.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

namespace mooncake {
namespace minimal {

using tcp = asio::ip::tcp;

// ── PersistentConn ────────────────────────────────────────────────────────────

struct PersistentConn {
    std::shared_ptr<asio::io_context>   ctx;
    std::unique_ptr<tcp::socket>        sock;
    std::mutex                          mu;   // serialize requests on this connection

    PersistentConn()
        : ctx(std::make_shared<asio::io_context>()),
          sock(std::make_unique<tcp::socket>(*ctx)) {}
};

// ── Lifecycle ─────────────────────────────────────────────────────────────────

TcpMinimalTransport::~TcpMinimalTransport() {
    uninstall();
}

int TcpMinimalTransport::install(const TransportConfig& /*config*/) {
    return 0;  // TCP requires no hardware setup
}

void TcpMinimalTransport::uninstall() {
    running_.store(false);
    if (ctrl_thread_.joinable()) ctrl_thread_.join();
    if (data_thread_.joinable()) data_thread_.join();

    std::lock_guard<std::mutex> lock(conns_mu_);
    conns_.clear();
}

int TcpMinimalTransport::init(const std::string& bind_ip, uint16_t ctrl_port) {
    bind_ip_   = bind_ip;
    ctrl_port_ = ctrl_port;
    data_port_ = ctrl_port + 1;
    running_.store(true);

    std::promise<void> ctrl_ready, data_ready;
    auto ctrl_fut = ctrl_ready.get_future();
    auto data_fut = data_ready.get_future();

    ctrl_thread_ = std::thread([this, p = std::move(ctrl_ready)]() mutable {
        runCtrlServer(std::move(p));
    });
    data_thread_ = std::thread([this, p = std::move(data_ready)]() mutable {
        runDataServer(std::move(p));
    });

    ctrl_fut.wait();
    data_fut.wait();
    return 0;
}

// ── Memory registration ───────────────────────────────────────────────────────

Status TcpMinimalTransport::registerMemory(void* addr, size_t length,
                                            MemRegion& out) {
    out = MemRegion{addr, length, 0, 0};
    std::lock_guard<std::mutex> lock(regions_mu_);
    local_regions_[addr] = out;
    return Status::OK();
}

void TcpMinimalTransport::unregisterMemory(void* addr) {
    std::lock_guard<std::mutex> lock(regions_mu_);
    local_regions_.erase(addr);
}

void TcpMinimalTransport::setMetaSearch(MetaSearch fn) {
    meta_search_ = std::move(fn);
}

// ── Persistent connections ────────────────────────────────────────────────────

Status TcpMinimalTransport::connectServer(const std::string& server_ip,
                                           uint16_t server_ctrl_port) {
    auto conn = std::make_shared<PersistentConn>();

    tcp::resolver resolver(*conn->ctx);
    asio::error_code ec;

    auto eps = resolver.resolve(server_ip, std::to_string(server_ctrl_port), ec);
    if (ec)
        return Status::Socket("connectServer resolve: " + ec.message());

    asio::connect(*conn->sock, eps, ec);
    if (ec)
        return Status::Socket("connectServer connect: " + ec.message());

    std::string key = server_ip + ":" + std::to_string(server_ctrl_port);
    std::lock_guard<std::mutex> lock(conns_mu_);
    conns_[key] = std::move(conn);
    return Status::OK();
}

void TcpMinimalTransport::disconnectServer(const std::string& server_ip,
                                            uint16_t server_ctrl_port) {
    std::string key = server_ip + ":" + std::to_string(server_ctrl_port);
    std::lock_guard<std::mutex> lock(conns_mu_);
    conns_.erase(key);
}

// ── Server ctrl acceptor ──────────────────────────────────────────────────────
//
// Accepts one connection at a time and loops over all ctrl requests from that
// connection until the client closes it.  This preserves the original
// synchronous model while supporting persistent client connections (established
// via connectServer) that issue multiple sequential request batches.

void TcpMinimalTransport::runCtrlServer(std::promise<void> ready) {
    asio::io_context ctx;
    tcp::acceptor    acceptor(ctx);
    tcp::endpoint    ep(tcp::v4(), ctrl_port_);
    asio::error_code ec;

    acceptor.open(ep.protocol(), ec);
    if (ec) { ready.set_value(); return; }
    acceptor.set_option(tcp::acceptor::reuse_address(true));
    acceptor.bind(ep, ec);
    if (ec) { ready.set_value(); return; }
    acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) { ready.set_value(); return; }

    // Non-blocking accept so running_ can be checked between calls.
    // SO_RCVTIMEO is unreliable for asio's kqueue-based accept on macOS.
    acceptor.non_blocking(true, ec);

    ready.set_value();

    while (running_.load()) {
        tcp::socket sock(ctx);
        acceptor.accept(sock, ec);
        if (ec == asio::error::would_block) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (ec) continue;

        // Accepted socket inherits non-blocking; restore blocking for sync I/O.
        sock.non_blocking(false, ec);

        asio::error_code io_ec;
        std::string client_ip =
            sock.remote_endpoint(io_ec).address().to_string();
        if (io_ec) continue;

        // Handle ONE ctrl request per connection (original behavior).
        ControlRequestHeader hdr{};
        asio::read(sock, asio::buffer(&hdr, sizeof(hdr)), io_ec);
        if (io_ec || hdr.magic != kControlMagic || hdr.count == 0) continue;

        std::vector<RequestEntry> entries(hdr.count);
        asio::read(sock,
                   asio::buffer(entries.data(),
                                hdr.count * sizeof(RequestEntry)),
                   io_ec);
        if (io_ec) continue;

        std::vector<ConcreteTransfer> xfers;
        int status_code = 0;
        for (const auto& e : entries) {
            MemRegion mr = meta_search_ ? meta_search_(e.key) : MemRegion{};
            if (!mr.addr) { status_code = ENOENT; break; }
            xfers.push_back({e.opcode, mr.addr, e.client_addr,
                             static_cast<size_t>(e.size)});
        }

        uint64_t transferred = 0;
        if (status_code == 0 && !xfers.empty())
            driveTransfers(client_ip, hdr.data_port, xfers, transferred);

        ControlResponse resp{kControlMagic, status_code, transferred};
        asio::write(sock, asio::buffer(&resp, sizeof(resp)), io_ec);
    }
}

// ── Client data acceptor ──────────────────────────────────────────────────────

void TcpMinimalTransport::runDataServer(std::promise<void> ready) {
    asio::io_context ctx;
    tcp::acceptor    acceptor(ctx);
    tcp::endpoint    ep(tcp::v4(), data_port_);
    asio::error_code ec;

    acceptor.open(ep.protocol(), ec);
    if (ec) { ready.set_value(); return; }
    acceptor.set_option(tcp::acceptor::reuse_address(true));
    acceptor.bind(ep, ec);
    if (ec) { ready.set_value(); return; }
    acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) { ready.set_value(); return; }

    acceptor.non_blocking(true, ec);

    ready.set_value();

    while (running_.load()) {
        tcp::socket sock(ctx);
        acceptor.accept(sock, ec);
        if (ec == asio::error::would_block) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (ec) continue;

        sock.non_blocking(false, ec);

        asio::error_code io_ec;
        while (true) {
            DataSessionHeader hdr{};
            asio::read(sock, asio::buffer(&hdr, sizeof(hdr)), io_ec);
            if (io_ec) break;

            char*  buf  = reinterpret_cast<char*>(hdr.addr);
            size_t size = static_cast<size_t>(hdr.size);

            if (hdr.opcode == 0) {
                asio::read(sock, asio::buffer(buf, size), io_ec);
            } else {
                asio::write(sock, asio::buffer(buf, size), io_ec);
            }
            if (io_ec) break;
        }
    }
}

// ── Server drives data transfer to client ────────────────────────────────────

void TcpMinimalTransport::driveTransfers(
    const std::string& client_ip, uint16_t data_port,
    const std::vector<ConcreteTransfer>& xfers,
    uint64_t& transferred_bytes) {

    asio::io_context ctx;
    tcp::socket      sock(ctx);
    tcp::resolver    resolver(ctx);
    asio::error_code ec;

    auto endpoints =
        resolver.resolve(client_ip, std::to_string(data_port), ec);
    if (ec) {
        LOG(WARNING) << "driveTransfers: resolve failed: " << ec.message();
        return;
    }

    asio::connect(sock, endpoints, ec);
    if (ec) {
        LOG(WARNING) << "driveTransfers: connect failed: " << ec.message();
        return;
    }

    for (const auto& xfer : xfers) {
        DataSessionHeader hdr{};
        hdr.size   = static_cast<uint64_t>(xfer.length);
        hdr.addr   = xfer.remote_addr;
        hdr.opcode = (xfer.opcode == OpCode::GET) ? 0 : 1;

        asio::write(sock, asio::buffer(&hdr, sizeof(hdr)), ec);
        if (ec) break;

        if (xfer.opcode == OpCode::GET) {
            asio::write(sock, asio::buffer(xfer.local_addr, xfer.length), ec);
        } else {
            asio::read(sock, asio::buffer(xfer.local_addr, xfer.length), ec);
        }
        if (!ec) transferred_bytes += xfer.length;
        else break;
    }

    // Graceful shutdown: signal EOF so the client's runDataServer finishes
    // reading before we send ControlResponse.
    sock.shutdown(tcp::socket::shutdown_send, ec);
    char drain[1];
    asio::read(sock, asio::buffer(drain, sizeof(drain)), ec);
    // ec == asio::error::eof is expected
}

// ── Client: submitAsync / pollBatch / freeBatch ───────────────────────────────

BatchID TcpMinimalTransport::submitAsync(
    const std::string& server_ip, uint16_t server_ctrl_port,
    const std::vector<RequestEntry>& entries) {

    if (entries.empty()) return kInvalidBatchID;

    auto    state = std::make_shared<BatchState>();
    BatchID bid   = reinterpret_cast<BatchID>(state.get());

    {
        std::lock_guard<std::mutex> lock(batches_mu_);
        batches_[bid] = state;
    }

    uint16_t data_port = data_port_;

    // Prefer a pre-established persistent connection.
    std::string key = server_ip + ":" + std::to_string(server_ctrl_port);
    std::shared_ptr<PersistentConn> conn;
    {
        std::lock_guard<std::mutex> lock(conns_mu_);
        auto it = conns_.find(key);
        if (it != conns_.end()) conn = it->second;
    }

    if (conn) {
        std::thread([entries, state, data_port, conn]() {
            // Serialize requests on the shared connection.
            std::lock_guard<std::mutex> lock(conn->mu);

            asio::error_code ec;
            ControlRequestHeader hdr{
                kControlMagic,
                static_cast<uint16_t>(entries.size()),
                data_port};

            asio::write(*conn->sock, asio::buffer(&hdr, sizeof(hdr)), ec);
            if (!ec) {
                asio::write(*conn->sock,
                            asio::buffer(entries.data(),
                                         entries.size() * sizeof(RequestEntry)),
                            ec);
            }
            if (ec) {
                state->status.store(TransferStatusEnum::FAILED,
                                    std::memory_order_release);
                return;
            }

            ControlResponse resp{};
            asio::read(*conn->sock, asio::buffer(&resp, sizeof(resp)), ec);
            if (ec || resp.magic != kControlMagic) {
                state->status.store(TransferStatusEnum::FAILED,
                                    std::memory_order_release);
                return;
            }

            state->transferred_bytes.store(resp.transferred_bytes,
                                           std::memory_order_relaxed);
            state->status.store(
                resp.status == 0 ? TransferStatusEnum::COMPLETED
                                 : TransferStatusEnum::FAILED,
                std::memory_order_release);
        }).detach();
    } else {
        // No persistent connection: open a new connection for this batch.
        std::thread([server_ip, server_ctrl_port, entries, state, data_port]() {
            asio::io_context ctx;
            tcp::socket      sock(ctx);
            tcp::resolver    resolver(ctx);
            asio::error_code ec;

            auto eps =
                resolver.resolve(server_ip, std::to_string(server_ctrl_port), ec);
            if (ec) {
                state->status.store(TransferStatusEnum::FAILED,
                                    std::memory_order_release);
                return;
            }

            asio::connect(sock, eps, ec);
            if (ec) {
                state->status.store(TransferStatusEnum::FAILED,
                                    std::memory_order_release);
                return;
            }

            ControlRequestHeader hdr{
                kControlMagic,
                static_cast<uint16_t>(entries.size()),
                data_port};

            asio::write(sock, asio::buffer(&hdr, sizeof(hdr)), ec);
            if (!ec) {
                asio::write(sock,
                            asio::buffer(entries.data(),
                                         entries.size() * sizeof(RequestEntry)),
                            ec);
            }
            if (ec) {
                state->status.store(TransferStatusEnum::FAILED,
                                    std::memory_order_release);
                return;
            }

            ControlResponse resp{};
            asio::read(sock, asio::buffer(&resp, sizeof(resp)), ec);
            if (ec || resp.magic != kControlMagic) {
                state->status.store(TransferStatusEnum::FAILED,
                                    std::memory_order_release);
                return;
            }

            state->transferred_bytes.store(resp.transferred_bytes,
                                            std::memory_order_relaxed);
            state->status.store(
                resp.status == 0 ? TransferStatusEnum::COMPLETED
                                 : TransferStatusEnum::FAILED,
                std::memory_order_release);
        }).detach();
    }

    return bid;
}

Status TcpMinimalTransport::pollBatch(BatchID id, TransferStatus& out) {
    std::shared_ptr<BatchState> state;
    {
        std::lock_guard<std::mutex> lock(batches_mu_);
        auto it = batches_.find(id);
        if (it == batches_.end())
            return Status::InvalidArgument("pollBatch: batch not found: " +
                                           std::to_string(id));
        state = it->second;
    }
    out.status = state->status.load(std::memory_order_acquire);
    out.transferred_bytes =
        state->transferred_bytes.load(std::memory_order_relaxed);
    return Status::OK();
}

Status TcpMinimalTransport::freeBatch(BatchID id) {
    std::lock_guard<std::mutex> lock(batches_mu_);
    if (batches_.erase(id) == 0)
        return Status::InvalidArgument("freeBatch: batch not found: " +
                                       std::to_string(id));
    return Status::OK();
}

}  // namespace minimal
}  // namespace mooncake
