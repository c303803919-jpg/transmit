// kv_server.cpp -- Server side of the three-round KV cache fetch.
// Implements Round-3 metadata stub and the gather DRAM->HBM transfer.
//
// Round-3 contract:
//   * Input: list of hash keys (one per miss group).
//   * Output: ThirdQueryEntry per group -> DRAM base address (or SSD LBA).
//   * Then for each group, gather (base + offset_i*kTokenSize) for every offset
//     in MissGroup.offsets and write contiguously to hbm_buffer.

#include "kv_transfer.h"

#include <cstring>
#include <iostream>

namespace kv_transfer {

// Mock server-side DRAM pool for round-3 lookup results.
namespace {
constexpr std::size_t kMockServerDramTokens = 8192;
char g_server_dram_pool[kMockServerDramTokens * kTokenSize];
}  // namespace

Server::Server() = default;

// ---------- Round 3 ----------
// Stubbed lookup: every hash is mapped to a deterministic offset in the mock
// server-side DRAM pool, marked as DRAM-resident.
std::vector<ThirdQueryEntry>
Server::third_query(const std::vector<std::uint64_t>& hash_keys) {
    std::vector<ThirdQueryEntry> out;
    out.reserve(hash_keys.size());

    for (std::uint64_t h : hash_keys) {
        ThirdQueryEntry e;
        // Reserve a 16-token window per group so offset 0..15 stay in-range.
        const std::size_t slot =
            (static_cast<std::size_t>(h) % (kMockServerDramTokens / kGroupSize))
            * kGroupSize;
        e.dram_addr = g_server_dram_pool + slot * kTokenSize;
        e.ssd_lba   = 0;
        e.is_dram   = true;
        out.push_back(e);
    }
    return out;
}

// ---------- Gather transfer ----------
bool Server::handle_transfer_request(const std::vector<std::uint64_t>& hash_keys,
                                     const std::vector<MissGroup>&     miss_groups,
                                     void*                              hbm_buffer) {
    if (hbm_buffer == nullptr) {
        std::cerr << "[Server] hbm_buffer is null\n";
        return false;
    }
    if (hash_keys.size() != miss_groups.size()) {
        std::cerr << "[Server] hash_keys / miss_groups size mismatch: "
                  << hash_keys.size() << " vs " << miss_groups.size() << '\n';
        return false;
    }

    auto entries = third_query(hash_keys);

    char* dst = static_cast<char*>(hbm_buffer);

    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        const auto& group = miss_groups[i];

        if (!entry.is_dram) {
            // Reserved: SSD path would issue a block read here.
            std::cerr << "[Server] SSD path not implemented (lba=" << entry.ssd_lba << ")\n";
            return false;
        }

        // Build gather source list: base + offset_i*kTokenSize for every offset.
        // (This works whether the underlying gather is one network primitive
        //  or a fan-out of independent DMAs.)
        std::vector<const void*> srcs;
        srcs.reserve(group.offsets.size());
        const char* base = static_cast<const char*>(entry.dram_addr);
        for (std::uint32_t off : group.offsets) {
            srcs.push_back(base + static_cast<std::size_t>(off) * kTokenSize);
        }

        if (!TransferEngine::gather_transfer(srcs, dst, kTokenSize)) {
            return false;
        }
        dst += group.offsets.size() * kTokenSize;
    }

    return true;
}

}  // namespace kv_transfer
