// kv_client.cpp -- Client side of the three-round KV cache fetch.
// Implements Round-1 / Round-2 metadata stubs and the DRAM->HBM transfer wrapper.

#include "kv_transfer.h"

#include <algorithm>
#include <cstring>
#include <iostream>

namespace kv_transfer {

// ----------------------------------------------------------------------------
// Mock local DRAM pool used by the round-1 stub. In a real system this is the
// node-local cache; here we just hand out deterministic addresses.
// ----------------------------------------------------------------------------
namespace {
constexpr std::size_t kMockDramTokens = 4096;
char g_local_dram_pool[kMockDramTokens * kTokenSize];
}  // namespace

// ============================================================================
// TransferEngine
// ============================================================================

bool TransferEngine::transfer_dram_to_hbm(const void* src, void* dst, std::size_t size) {
    if (src == nullptr || dst == nullptr || size == 0) {
        return false;
    }
    // In a real system this would issue a DMA descriptor to the NPU HBM.
    // For tests "HBM" is just another piece of memory.
    std::memcpy(dst, src, size);
    return true;
}

bool TransferEngine::gather_transfer(const std::vector<const void*>& srcs,
                                     void*                            dst,
                                     std::size_t                      per_size) {
    if (dst == nullptr || per_size == 0) {
        return false;
    }
    char* d = static_cast<char*>(dst);
    for (const void* s : srcs) {
        if (s == nullptr) {
            return false;
        }
        std::memcpy(d, s, per_size);
        d += per_size;
    }
    return true;
}

// ============================================================================
// Client
// ============================================================================

Client::Client() = default;

// ---------- Round 1 ----------
// Stub policy (returned as default per requirement):
//   * even tokenid  => hit  (mapped into g_local_dram_pool)
//   * odd  tokenid  => miss
// Misses are then grouped by intervals of kGroupSize (=16) starting at
// each first remaining miss; within a group we record:
//   - first_tokenid (group base)
//   - bitmap of which slots in [base, base+16) are misses
//   - differential offsets (offsets[0] is always 0)
FirstQueryResult Client::first_query(const Key& key) {
    FirstQueryResult result;
    result.aggregated_key.reqid    = key.reqid;
    result.aggregated_key.layerid  = key.layerid;

    std::vector<std::uint32_t> miss_tokens;
    miss_tokens.reserve(key.tokenids.size());

    for (std::uint32_t tid : key.tokenids) {
        const bool is_hit = ((tid & 0x1u) == 0u);  // default stub: even = hit
        if (is_hit) {
            HitEntry e;
            e.tokenid   = tid;
            e.dram_addr = g_local_dram_pool + (tid % kMockDramTokens) * kTokenSize;
            result.hits.push_back(e);
        } else {
            miss_tokens.push_back(tid);
        }
    }

    // Group miss tokens. Inputs are discrete (0..16 per group) so we walk the
    // sorted miss list and start a new group whenever a tid falls outside the
    // current 16-slot window.
    std::size_t i = 0;
    while (i < miss_tokens.size()) {
        MissGroup g;
        g.first_tokenid = miss_tokens[i];
        g.bitmap        = static_cast<std::uint16_t>(0x1u);  // bit 0 = first
        g.offsets.push_back(0u);

        std::size_t j = i + 1;
        while (j < miss_tokens.size()) {
            const std::uint32_t diff = miss_tokens[j] - g.first_tokenid;
            if (diff >= kGroupSize) break;
            g.offsets.push_back(diff);
            g.bitmap = static_cast<std::uint16_t>(g.bitmap | (1u << diff));
            ++j;
        }

        result.aggregated_key.tokenids_unmap.push_back(g.first_tokenid);
        result.miss_groups.push_back(std::move(g));
        i = j;
    }

    return result;
}

// ---------- DRAM -> HBM for hits ----------
bool Client::process_hits(const std::vector<HitEntry>& hits, void* hbm_buffer) {
    if (hbm_buffer == nullptr) {
        return false;
    }
    char* dst = static_cast<char*>(hbm_buffer);
    for (const auto& h : hits) {
        if (!TransferEngine::transfer_dram_to_hbm(h.dram_addr, dst, kTokenSize)) {
            return false;
        }
        dst += kTokenSize;
    }
    return true;
}

// ---------- Round 2 ----------
// Stub: produce a synthetic 64-bit hash and always claim local IP.
std::vector<SecondQueryEntry> Client::second_query(const Key1& key1) {
    std::vector<SecondQueryEntry> out;
    out.reserve(key1.tokenids_unmap.size());

    // Cheap, deterministic hash combining reqid/layerid/tokenid.
    // Real implementation would call into the metadata service.
    const std::uint64_t reqid_hash = std::hash<std::string>{}(key1.reqid);
    for (std::uint32_t tid : key1.tokenids_unmap) {
        SecondQueryEntry e;
        e.hash_key      = reqid_hash
                        ^ (static_cast<std::uint64_t>(key1.layerid) << 32)
                        ^ static_cast<std::uint64_t>(tid);
        e.ip_address    = kLocalIp;          // default: local
        e.first_tokenid = tid;
        out.push_back(e);
    }
    return out;
}

bool Client::is_local_ip(const std::string& ip) {
    return ip == kLocalIp || ip == "localhost";
}

// Reserved: would issue an NoF / RDMA two-sided GET against entry.ip_address.
bool Client::remote_transfer(const SecondQueryEntry& /*entry*/, void* /*dst*/) {
    std::cerr << "[Client] remote_transfer: not implemented in this drop "
                 "(reserved for NoF/RDMA two-sided path)\n";
    return false;
}

}  // namespace kv_transfer
