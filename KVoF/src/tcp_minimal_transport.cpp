#include "tcp_minimal_transport.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <glog/logging.h>
#include <unistd.h>

#include <asio/connect.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

namespace mooncake {
namespace minimal {

using tcp  = asio::ip::tcp;
using Clk  = std::chrono::steady_clock;
using Ms   = std::chrono::milliseconds;

// ── PersistentConn ────────────────────────────────────────────────────────────

struct PersistentConn {
    std::shared_ptr<asio::io_context> ctx;
    std::unique_ptr<tcp::socket>      sock;
    std::mutex                        mu;

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
    LOG(INFO) << "[transport] init done: bind=" << bind_ip
              << " ctrl_port=" << ctrl_port;
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
    LOG(INFO) << "[transport] connectServer " << server_ip
              << ":" << server_ctrl_port;

    auto conn = std::make_shared<PersistentConn>();
    tcp::resolver resolver(*conn->ctx);
    asio::error_code ec;

    auto eps = resolver.resolve(server_ip, std::to_string(server_ctrl_port), ec);
    if (ec) {
        LOG(WARNING) << "[transport] connectServer resolve failed: " << ec.message();
        return Status::Socket("connectServer resolve: " + ec.message());
    }

    asio::connect(*conn->sock, eps, ec);
    if (ec) {
        LOG(WARNING) << "[transport] connectServer connect failed: " << ec.message();
        return Status::Socket("connectServer connect: " + ec.message());
    }

    LOG(INFO) << "[transport] connectServer OK → " << server_ip
              << ":" << server_ctrl_port;

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
    LOG(INFO) << "[transport] disconnectServer " << server_ip
              << ":" << server_ctrl_port;
}

// ── Inline data exchange helper ───────────────────────────────────────────────
//
// All control + data exchange happens on a single TCP socket.
// Server sends one DataSessionHeader per entry, then data bytes for ok entries.
// Client reads each DataSessionHeader and mirrors the data exchange.

template <typename Socket>
static bool exchangeData(Socket& sock,
                         const std::vector<RequestEntry>& entries,
                         bool is_server,
                         const std::string& peer_label,
                         std::function<MemRegion(uint64_t)> meta_search,
                         int& status_code_out,
                         uint64_t& transferred_out) {
    asio::error_code ec;
    bool io_broken = false;

    for (size_t idx = 0; idx < entries.size(); ++idx) {
        const auto& e = entries[idx];

        if (is_server) {
            // ── Server: resolve key, send DataSessionHeader, transfer data ────
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

            LOG(INFO) << "[server] entry[" << idx << "] key=" << e.key
                      << " opcode=" << (e.opcode == OpCode::GET ? "GET" : "PUT")
                      << " meta=" << (mr.addr ? "HIT" : "MISS")
                      << " → DSH{size=" << dsh.size
                      << " entry_status=" << dsh.entry_status
                      << " opcode=" << static_cast<int>(dsh.opcode) << "}";

            asio::write(sock, asio::buffer(&dsh, sizeof(dsh)), ec);
            if (ec) {
                LOG(WARNING) << "[server] write DSH failed: " << ec.message();
                io_broken = true;
                continue;
            }

            if (dsh.entry_status != 0) continue;

            auto t0 = Clk::now();
            char* local_buf = static_cast<char*>(mr.addr);
            if (e.opcode == OpCode::GET) {
                asio::write(sock, asio::buffer(local_buf, e.size), ec);
            } else {
                asio::read(sock, asio::buffer(local_buf, e.size), ec);
            }
            auto elapsed = std::chrono::duration_cast<Ms>(Clk::now() - t0).count();

            if (ec) {
                LOG(WARNING) << "[server] data I/O failed entry[" << idx
                             << "]: " << ec.message();
                io_broken = true;
                if (status_code_out == 0) status_code_out = EIO;
            } else {
                transferred_out += e.size;
                LOG(INFO) << "[server] entry[" << idx << "] "
                          << (e.opcode == OpCode::GET ? "GET sent" : "PUT recv")
                          << " " << e.size << " bytes in " << elapsed << "ms";
            }

        } else {
            // ── Client: read DataSessionHeader, then exchange data ─────────────
            if (io_broken) break;

            DataSessionHeader dsh{};
            asio::read(sock, asio::buffer(&dsh, sizeof(dsh)), ec);
            if (ec) {
                LOG(WARNING) << "[client] read DSH failed entry[" << idx
                             << "] peer=" << peer_label
                             << ": " << ec.message();
                io_broken = true;
                break;
            }

            LOG(INFO) << "[client] entry[" << idx
                      << "] DSH{size=" << dsh.size
                      << " entry_status=" << dsh.entry_status
                      << " opcode=" << static_cast<int>(dsh.opcode) << "}"
                      << " peer=" << peer_label;

            if (dsh.entry_status != 0) {
                LOG(WARNING) << "[client] entry[" << idx
                             << "] server error entry_status="
                             << dsh.entry_status << " ("
                             << strerror(static_cast<int>(dsh.entry_status))
                             << ")";
                continue;
            }

            auto t0 = Clk::now();
            void* client_buf = reinterpret_cast<void*>(e.client_addr);
            if (dsh.opcode == 0) {
                asio::read(sock, asio::buffer(client_buf, e.size), ec);
            } else {
                asio::write(sock, asio::buffer(client_buf, e.size), ec);
            }
            auto elapsed = std::chrono::duration_cast<Ms>(Clk::now() - t0).count();

            if (ec) {
                LOG(WARNING) << "[client] data I/O failed entry[" << idx
                             << "] peer=" << peer_label
                             << ": " << ec.message();
                io_broken = true;
            } else {
                LOG(INFO) << "[client] entry[" << idx << "] "
                          << (dsh.opcode == 0 ? "GET recv" : "PUT sent")
                          << " " << e.size << " bytes in " << elapsed << "ms"
                          << " peer=" << peer_label;
            }
        }
    }

    return !io_broken;
}

// ── Server ctrl acceptor ──────────────────────────────────────────────────────

void TcpMinimalTransport::runCtrlServer(std::promise<void> ready) {
    asio::io_context ctx;
    tcp::acceptor    acceptor(ctx);
    tcp::endpoint    ep(tcp::v4(), ctrl_port_);
    asio::error_code ec;

    acceptor.open(ep.protocol(), ec);
    if (ec) {
        LOG(ERROR) << "[server] acceptor open failed: " << ec.message();
        ready.set_value(); return;
    }
    acceptor.set_option(tcp::acceptor::reuse_address(true));
    acceptor.bind(ep, ec);
    if (ec) {
        LOG(ERROR) << "[server] bind " << ctrl_port_
                   << " failed: " << ec.message();
        ready.set_value(); return;
    }
    acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        LOG(ERROR) << "[server] listen failed: " << ec.message();
        ready.set_value(); return;
    }

    acceptor.non_blocking(true, ec);
    LOG(INFO) << "[server] ctrl acceptor listening on port " << ctrl_port_;
    ready.set_value();

    while (running_.load()) {
        tcp::socket sock(ctx);
        acceptor.accept(sock, ec);
        if (ec == asio::error::would_block) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (ec) {
            LOG(WARNING) << "[server] accept error: " << ec.message();
            continue;
        }

        sock.non_blocking(false, ec);

        asio::error_code io_ec;
        std::string client_ip = "unknown";
        auto ep_remote = sock.remote_endpoint(io_ec);
        if (!io_ec) {
            client_ip = ep_remote.address().to_string() + ":" +
                        std::to_string(ep_remote.port());
        }
        LOG(INFO) << "[server] accepted connection from " << client_ip;

        // Inner loop: handle multiple batches on the same connection.
        while (true) {
            ControlRequestHeader hdr{};
            asio::read(sock, asio::buffer(&hdr, sizeof(hdr)), io_ec);
            if (io_ec) {
                if (io_ec != asio::error::eof)
                    LOG(WARNING) << "[server] read ctrl hdr from " << client_ip
                                 << ": " << io_ec.message();
                break;
            }

            if (hdr.magic != kControlMagic) {
                LOG(WARNING) << "[server] bad magic 0x" << std::hex << hdr.magic
                             << std::dec << " from " << client_ip;
                break;
            }

            LOG(INFO) << "[server] ctrl request from " << client_ip
                      << " count=" << hdr.count;

            if (hdr.count == 0) break;

            std::vector<RequestEntry> entries(hdr.count);
            asio::read(sock,
                       asio::buffer(entries.data(),
                                    hdr.count * sizeof(RequestEntry)),
                       io_ec);
            if (io_ec) {
                LOG(WARNING) << "[server] read entries failed: " << io_ec.message();
                break;
            }

            int      status_code = 0;
            uint64_t transferred = 0;

            bool ok = exchangeData(sock, entries, /*is_server=*/true,
                                   client_ip, meta_search_,
                                   status_code, transferred);
            if (!ok) break;

            ControlResponse resp{kControlMagic, status_code, transferred};
            LOG(INFO) << "[server] → ControlResponse status=" << status_code
                      << " transferred=" << transferred
                      << " to " << client_ip;
            asio::write(sock, asio::buffer(&resp, sizeof(resp)), io_ec);
            if (io_ec) {
                LOG(WARNING) << "[server] write response failed: " << io_ec.message();
                break;
            }
        }

        LOG(INFO) << "[server] connection closed: " << client_ip;
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

    std::string server_label = server_ip + ":" + std::to_string(server_ctrl_port);

    std::string key = server_label;
    std::shared_ptr<PersistentConn> conn;
    {
        std::lock_guard<std::mutex> lock(conns_mu_);
        auto it = conns_.find(key);
        if (it != conns_.end()) conn = it->second;
    }

    auto run = [entries, state, server_label](auto& sock) {
        asio::error_code ec;

        ControlRequestHeader hdr{kControlMagic,
                                  static_cast<uint16_t>(entries.size()),
                                  0 /*data_port unused*/};

        LOG(INFO) << "[client] → ControlRequestHeader count=" << hdr.count
                  << " server=" << server_label;

        asio::write(sock, asio::buffer(&hdr, sizeof(hdr)), ec);
        if (!ec) {
            asio::write(sock,
                        asio::buffer(entries.data(),
                                     entries.size() * sizeof(RequestEntry)),
                        ec);
        }
        if (ec) {
            LOG(WARNING) << "[client] send ctrl request failed: " << ec.message()
                         << " server=" << server_label;
            state->status.store(TransferStatusEnum::FAILED,
                                std::memory_order_release);
            return;
        }

        int      status_code = 0;
        uint64_t transferred = 0;

        bool ok = exchangeData(sock, entries, /*is_server=*/false,
                               server_label, nullptr,
                               status_code, transferred);
        if (!ok) {
            state->status.store(TransferStatusEnum::FAILED,
                                std::memory_order_release);
            return;
        }

        ControlResponse resp{};
        asio::read(sock, asio::buffer(&resp, sizeof(resp)), ec);
        if (ec) {
            LOG(WARNING) << "[client] read ControlResponse failed: " << ec.message()
                         << " server=" << server_label;
            state->status.store(TransferStatusEnum::FAILED,
                                std::memory_order_release);
            return;
        }
        if (resp.magic != kControlMagic) {
            LOG(WARNING) << "[client] bad response magic 0x"
                         << std::hex << resp.magic << std::dec
                         << " server=" << server_label;
            state->status.store(TransferStatusEnum::FAILED,
                                std::memory_order_release);
            return;
        }

        LOG(INFO) << "[client] ← ControlResponse status=" << resp.status
                  << " transferred=" << resp.transferred_bytes
                  << " server=" << server_label;

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
        std::thread([server_ip, server_ctrl_port, server_label, run, state]() {
            asio::io_context ctx;
            tcp::socket      sock(ctx);
            tcp::resolver    resolver(ctx);
            asio::error_code ec;

            LOG(INFO) << "[client] connecting to " << server_label;
            auto eps = resolver.resolve(
                server_ip, std::to_string(server_ctrl_port), ec);
            if (ec) {
                LOG(WARNING) << "[client] resolve failed: " << ec.message()
                             << " server=" << server_label;
                state->status.store(TransferStatusEnum::FAILED,
                                    std::memory_order_release);
                return;
            }
            asio::connect(sock, eps, ec);
            if (ec) {
                LOG(WARNING) << "[client] connect failed: " << ec.message()
                             << " server=" << server_label;
                state->status.store(TransferStatusEnum::FAILED,
                                    std::memory_order_release);
                return;
            }

            asio::error_code ep_ec;
            auto local_ep = sock.local_endpoint(ep_ec);
            if (!ep_ec)
                LOG(INFO) << "[client] connected " << server_label
                          << " local=" << local_ep.address().to_string()
                          << ":" << local_ep.port();

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
