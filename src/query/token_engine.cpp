// token_engine.cpp -- Engine + token_get_index implementation.
//
// The flow exercised by token_get_index:
//
//   round 1:  Engine::batchQuery(key)             (client side, exactly one call)
//             -> vector<MetaInfo>
//             per entry:
//                kVa          -> va_to_hbm(...)            (empty stub)
//                kLba         -> lba_to_hbm(...)           (reserved)
//                kIpTokenkey  -> defer (collect tokenkey)
//
//   round 2:  Engine::batchQueryLocal(tokenkeys)  (server side, at most one call)
//             -> vector<MetaInfo>                          (always kVa in stub)
//             per entry:
//                kVa  -> va_to_hbm(...)
//                kLba -> lba_to_hbm(...)                   (reserved)
//
// The metadata stubs only return defaults: batchQuery picks VA vs IP+tokenkey
// per-token (random by default, configurable for tests); batchQueryLocal
// always returns a kVa pointing at a fixed mock pool.

#include "kv_transfer.h"

#include <iostream>

namespace kv_transfer {

namespace {

// ---------------------------------------------------------------------------
// Mock DRAM pools used by the stubs. Each tokenid / tokenkey is mapped to a
// deterministic page so that callers see a stable VA across runs.
// ---------------------------------------------------------------------------
constexpr std::size_t kMockClientDramTokens = 4096;
constexpr std::size_t kMockServerDramTokens = 8192;

char g_mock_client_dram[kMockClientDramTokens * kTokenSize];
char g_mock_server_dram[kMockServerDramTokens * kTokenSize];

void* mock_client_va(std::uint32_t tid) {
    return g_mock_client_dram +
           (tid % kMockClientDramTokens) * kTokenSize;
}

void* mock_server_va(std::uint64_t tk) {
    return g_mock_server_dram +
           (tk % kMockServerDramTokens) * kTokenSize;
}

std::uint64_t make_tokenkey(const std::string& reqid,
                            std::uint32_t      layerid,
                            std::uint32_t      tid) {
    return std::hash<std::string>{}(reqid)
         ^ (static_cast<std::uint64_t>(layerid) << 32)
         ^ static_cast<std::uint64_t>(tid);
}

constexpr const char* kRemoteIp = "10.0.0.42";

}  // namespace

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

Engine::Engine() = default;

void Engine::set_query_stub(QueryStubMode mode)        { stub_mode_  = mode; }
void Engine::set_seed(std::uint64_t seed)              { rng_.seed(seed); }
void Engine::set_token_size(std::size_t s)             { token_size_ = s; }
void Engine::set_hbm(void* buf, std::size_t cap) {
    hbm_buf_      = buf;
    hbm_capacity_ = cap;
}

void Engine::reset_counters() {
    cnt_batch_query_       = 0;
    cnt_batch_query_local_ = 0;
    cnt_va_to_hbm_         = 0;
    cnt_lba_to_hbm_        = 0;
}

// ---------- batchQuery (client) ----------
// Simple stub:
//   * kAlwaysVa     -> every token is a local VA.
//   * kAlwaysIp     -> every token is remote (IP + tokenkey).
//   * kAlwaysLba    -> every token is an SSD LBA (test-only path).
//   * kAlternating  -> even idx -> VA, odd idx -> IP+tokenkey (deterministic).
//   * kRandom       -> 50/50 VA vs IP per token.
std::vector<MetaInfo> Engine::batchQuery(const Key& key) {
    cnt_batch_query_ += 1;

    std::vector<MetaInfo> out;
    out.reserve(key.tokenids.size());
    for (std::size_t i = 0; i < key.tokenids.size(); ++i) {
        const std::uint32_t tid = key.tokenids[i];
        MetaInfo m;
        m.tokenid = tid;

        bool emit_va  = false;
        bool emit_lba = false;
        bool emit_ip  = false;
        switch (stub_mode_) {
            case QueryStubMode::kAlwaysVa:    emit_va  = true; break;
            case QueryStubMode::kAlwaysIp:    emit_ip  = true; break;
            case QueryStubMode::kAlwaysLba:   emit_lba = true; break;
            case QueryStubMode::kAlternating:
                (i % 2 == 0) ? emit_va = true : emit_ip = true;
                break;
            case QueryStubMode::kRandom:
            default:
                ((rng_() & 1ULL) == 0ULL) ? emit_va = true : emit_ip = true;
                break;
        }

        if (emit_va) {
            m.type = MetaInfoType::kVa;
            m.va   = mock_client_va(tid);
        } else if (emit_lba) {
            m.type = MetaInfoType::kLba;
            // Arbitrary deterministic LBA mapping; reserved interface.
            m.lba  = static_cast<std::uint64_t>(tid) * 8ULL;
        } else {  // emit_ip
            m.type     = MetaInfoType::kIpTokenkey;
            m.ip       = kRemoteIp;
            m.tokenkey = make_tokenkey(key.reqid, key.layerid, tid);
        }
        out.push_back(std::move(m));
    }
    return out;
}

// ---------- batchQueryLocal (server) ----------
// Stub: always returns kVa pointing at a fixed mock pool slot derived from
// the tokenkey. Real implementation would call the metadata service.
std::vector<MetaInfo>
Engine::batchQueryLocal(const std::vector<std::uint64_t>& tokenkeys) {
    cnt_batch_query_local_ += 1;

    std::vector<MetaInfo> out;
    out.reserve(tokenkeys.size());
    for (std::uint64_t tk : tokenkeys) {
        MetaInfo m;
        m.type     = MetaInfoType::kVa;
        m.tokenkey = tk;
        m.va       = mock_server_va(tk);
        out.push_back(std::move(m));
    }
    return out;
}

// ---------- transfer primitives ----------
// Empty stubs per requirement: VA path returns 0 = success; LBA path is the
// reserved interface for the future SSD reader.
int Engine::va_to_hbm(const void* /*va*/, void* /*hbm*/, std::size_t /*size*/) {
    cnt_va_to_hbm_ += 1;
    return 0;
}

int Engine::lba_to_hbm(std::uint64_t /*lba*/, void* /*hbm*/, std::size_t /*size*/) {
    cnt_lba_to_hbm_ += 1;
    // Reserved -- SSD reader not implemented yet.
    return 0;
}

// ---------- token_get_index ----------
namespace {

// Dispatch a single MetaInfo to the right transfer primitive.
// `slot` may be nullptr if no HBM buffer was configured -- the stubs ignore it.
// `allow_remote` is true on round 1 (remote entries are deferred), false on
// round 2 (the server side must not bounce back to a remote tier).
bool dispatch_transfer(Engine&         engine,
                       const MetaInfo& m,
                       void*           slot,
                       std::size_t     token_size,
                       bool            allow_remote,
                       bool*           deferred /*opt out*/) {
    if (deferred != nullptr) {
        *deferred = false;
    }
    switch (m.type) {
        case MetaInfoType::kVa:
            return engine.va_to_hbm(m.va, slot, token_size) == 0;

        case MetaInfoType::kLba:
            return engine.lba_to_hbm(m.lba, slot, token_size) == 0;

        case MetaInfoType::kIpTokenkey:
            if (!allow_remote) {
                std::cerr << "[Engine] unexpected kIpTokenkey on round 2\n";
                return false;
            }
            if (deferred != nullptr) {
                *deferred = true;
            }
            return true;

        case MetaInfoType::kInvalid:
        default:
            std::cerr << "[Engine] invalid MetaInfo type "
                      << static_cast<unsigned>(m.type) << '\n';
            return false;
    }
}

}  // namespace

bool Engine::token_get_index(const Key& key) {
    char* hbm_base = static_cast<char*>(hbm_buf_);  // may be null

    // Round 1 -- one client-side query.
    auto round1 = batchQuery(key);
    if (round1.size() != key.tokenids.size()) {
        std::cerr << "[Engine] batchQuery returned " << round1.size()
                  << " entries, expected " << key.tokenids.size() << '\n';
        return false;
    }

    std::vector<std::uint64_t> deferred_tokenkeys;
    std::vector<std::size_t>   deferred_indices;
    deferred_tokenkeys.reserve(round1.size());
    deferred_indices.reserve(round1.size());

    for (std::size_t i = 0; i < round1.size(); ++i) {
        char* slot = (hbm_base != nullptr) ? hbm_base + i * token_size_
                                           : nullptr;
        bool deferred = false;
        if (!dispatch_transfer(*this, round1[i], slot, token_size_,
                               /*allow_remote=*/true, &deferred)) {
            return false;
        }
        if (deferred) {
            deferred_tokenkeys.push_back(round1[i].tokenkey);
            deferred_indices.push_back(i);
        }
    }

    if (deferred_tokenkeys.empty()) {
        return true;
    }

    // Round 2 -- one server-side query for everything the client deferred.
    auto round2 = batchQueryLocal(deferred_tokenkeys);
    if (round2.size() != deferred_tokenkeys.size()) {
        std::cerr << "[Engine] batchQueryLocal returned " << round2.size()
                  << " entries, expected " << deferred_tokenkeys.size() << '\n';
        return false;
    }

    for (std::size_t k = 0; k < round2.size(); ++k) {
        const std::size_t slot_idx = deferred_indices[k];
        char* slot = (hbm_base != nullptr) ? hbm_base + slot_idx * token_size_
                                           : nullptr;
        if (!dispatch_transfer(*this, round2[k], slot, token_size_,
                               /*allow_remote=*/false, nullptr)) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Free-function entry points
// ---------------------------------------------------------------------------

bool token_get_index(const Key& key) {
    Engine engine;
    return engine.token_get_index(key);
}

bool token_get_index(const Key& key, Engine& engine) {
    return engine.token_get_index(key);
}

}  // namespace kv_transfer
