#include "tcp_minimal_transport.h"

#include <cerrno>
#include <glog/logging.h>
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
    std::shared_ptr<asio::io_context> ctx;
    std::unique_ptr<tcp::socket>      sock;
    std::mutex                        mu;  // serialize requests on this connection

    PersistentConn()
        : ctx(std::make_shared<asio::io_context>()),
          sock(std::make_unique<tcp::socket>(*ctx)) {}
};

// ── Lifecycle ─────────────────────────────────────────────────────────────────

TcpMinimalTransport::~TcpMinimalTransport() {
    uninstall();
}

int TcpMinimalTransport::install(const TransportConfig& /*config*/) {
    return 0;
}

void TcpMinimalTransport::uninstall() {
    running_.store(false);
    if (ctrl_thread_.joinable()) ctrl_thread_.join();

    std::lock_guard<std::mutex> lock(conns_mu_);
    conns_.clear();
}

int TcpMinimalTransport::init(const std::string& bind_ip, uint16_t ctrl_port) {
    bind_ip_   = bind_ip;
    ctrl_port_ = ctrl_port;
    running_.store(true);

    std::promise<void> ctrl_ready;
    auto ctrl_fut = ctrl_ready.get_future();

    ctrl_thread_ = std::thread([this, p = std::move(ctrl_ready)]() mutable {
        runCtrlServer(std::move(p));
    });

    ctrl_fut.wait();
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
    if (ec) return Status::Socket("connectServer resolve: " + ec.message());

    asio::connect(*conn->sock, eps, ec);
    if (ec) return Status::Socket("connectServer connect: " + ec.message());

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

// ── Inline data helper ────────────────────────────────────────────────────────
//
// Called by both the server (runCtrlServer) and the client (submitAsync) to
// perform per-entry data exchange on the shared ctrl TCP socket.
//
// Server side (is_server=true):
//   - Sends DataSessionHeader per entry.
//   - GET: writes slot bytes to socket.
//   - PUT: reads socket bytes into slot.
//
// Client side (is_server=false):
//   - Reads DataSessionHeader per entry.
//   - GET: reads socket bytes into client_addr buffer.
//   - PUT: writes client_addr buffer bytes to socket.

template <typename Socket>
static bool exchangeData(Socket& sock,
                         const std::vector<RequestEntry>& entries,
                         bool is_server,
                         std::function<MemRegion(uint64_t)> meta_search,
                         int& status_code_out,
                         uint64_t& transferred_out) {
    asio::error_code ec;
    bool io_broken = false;

    for (const auto& e : entries) {
        // ── Server: build and send DataSessionHeader ──────────────────────────
        if (is_server) {
            MemRegion mr{};
            if (!io_broken && status_code_out == 0 && meta_search)
                mr = meta_search(e.key);
            if (!mr.addr && status_code_out == 0)
                status_code_out = ENOENT;

            DataSessionHeader dsh{};
            dsh.opcode = (e.opcode == OpCode::GET) ? 0 : 1;
            if (io_broken || status_code_out != 0 || !mr.addr) {
                dsh.size         = 0;
                dsh.entry_status = static_cast<uint64_t>(
                    status_code_out ? status_code_out : ENOENT);
            } else {
                dsh.size         = e.size;
                dsh.entry_status = 0;
            }

            asio::write(sock, asio::buffer(&dsh, sizeof(dsh)), ec);
            if (ec) { io_broken = true; continue; }

            if (dsh.entry_status != 0) continue;  // error entry — no data

            char* local_buf = static_cast<char*>(mr.addr);
            if (e.opcode == OpCode::GET) {
                asio::write(sock, asio::buffer(local_buf, e.size), ec);
            } else {
                asio::read(sock, asio::buffer(local_buf, e.size), ec);
            }
            if (ec) {
                io_broken = true;
                if (status_code_out == 0) status_code_out = EIO;
            } else {
                transferred_out += e.size;
            }

        // ── Client: read DataSessionHeader and do data exchange ───────────────
        } else {
            if (io_broken) break;

            DataSessionHeader dsh{};
            asio::read(sock, asio::buffer(&dsh, sizeof(dsh)), ec);
            if (ec) { io_broken = true; break; }

            if (dsh.entry_status != 0) continue;  // server-reported error, no data

            void* client_buf = reinterpret_cast<void*>(e.client_addr);
            if (dsh.opcode == 0) {  // GET: receive from server
                asio::read(sock, asio::buffer(client_buf, e.size), ec);
            } else {                // PUT: send to server
                asio::write(sock, asio::buffer(client_buf, e.size), ec);
            }
            if (ec) io_broken = true;
        }
    }

    return !io_broken;
}

// ── Server ctrl acceptor ──────────────────────────────────────────────────────
//
// All data exchange (GET bytes / PUT bytes) happens inline on the ctrl socket.
// No reverse connection to the client is needed; the client initiates all TCP
// connections so firewalls and NAT are never an issue.
//
// Multiple request batches from the same client connection are handled in the
// inner while(true) loop, which exits when the client disconnects (EOF).

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

    // Non-blocking accept so running_ can be polled between accepts.
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

        // Restore blocking I/O on the accepted socket.
        sock.non_blocking(false, ec);

        asio::error_code io_ec;

        // Inner loop: handle multiple batches on the same TCP connection
        // (used by persistent clients). Single-shot clients break on EOF.
        while (true) {
            ControlRequestHeader hdr{};
            asio::read(sock, asio::buffer(&hdr, sizeof(hdr)), io_ec);
            if (io_ec) break;
            if (hdr.magic != kControlMagic || hdr.count == 0) break;

            std::vector<RequestEntry> entries(hdr.count);
            asio::read(sock,
                       asio::buffer(entries.data(),
                                    hdr.count * sizeof(RequestEntry)),
                       io_ec);
            if (io_ec) break;

            int      status_code = 0;
            uint64_t transferred = 0;

            bool ok = exchangeData(sock, entries, /*is_server=*/true,
                                   meta_search_, status_code, transferred);

            if (!ok) break;

            ControlResponse resp{kControlMagic, status_code, transferred};
            asio::write(sock, asio::buffer(&resp, sizeof(resp)), io_ec);
            if (io_ec) break;
        }
    }
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

    // Prefer a pre-established persistent connection.
    std::string key = server_ip + ":" + std::to_string(server_ctrl_port);
    std::shared_ptr<PersistentConn> conn;
    {
        std::lock_guard<std::mutex> lock(conns_mu_);
        auto it = conns_.find(key);
        if (it != conns_.end()) conn = it->second;
    }

    auto run = [entries, state](auto& sock) {
        asio::error_code ec;

        ControlRequestHeader hdr{kControlMagic,
                                  static_cast<uint16_t>(entries.size()),
                                  0 /*data_port unused*/};
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

        int      status_code = 0;
        uint64_t transferred = 0;
        bool ok = exchangeData(sock, entries, /*is_server=*/false,
                               nullptr, status_code, transferred);
        if (!ok) {
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
    };

    if (conn) {
        std::thread([run, state, conn]() {
            std::lock_guard<std::mutex> lock(conn->mu);
            run(*conn->sock);
        }).detach();
    } else {
        std::thread([server_ip, server_ctrl_port, run, state]() {
            asio::io_context ctx;
            tcp::socket      sock(ctx);
            tcp::resolver    resolver(ctx);
            asio::error_code ec;

            auto eps = resolver.resolve(
                server_ip, std::to_string(server_ctrl_port), ec);
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
            run(sock);
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
