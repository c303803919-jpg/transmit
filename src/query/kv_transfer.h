// kv_transfer.h
// Three-round metadata-query + data-transfer skeleton
// Round 1 (Client): linear keys -> hit list (local DRAM) + miss list (grouped w/ bitmap)
// Round 2 (Client): aggregated miss key -> hash key + ip (local/remote)
// Round 3 (Server): hash key -> DRAM addr / SSD LBA, gather DRAM->HBM
//
// Notes:
//   * Metadata lookups are stubs that return defaults (per requirement).
//   * "HBM" is simulated by a plain memory buffer in tests.
//   * Token size is configurable (default 576B).

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <functional>

namespace kv_transfer {

// ---------- configurable constants ----------
constexpr std::size_t kTokenSize = 576;       // single-token byte size, configurable
constexpr std::size_t kGroupSize = 16;        // miss-grouping interval (bitmap width)
constexpr const char* kLocalIp   = "127.0.0.1";

// ---------- keys ----------

// Round-1 input key: (reqid, layerid, [tokenid...] sorted ascending)
struct Key {
    std::string           reqid;
    std::uint32_t         layerid {0};
    std::vector<std::uint32_t> tokenids;   // sorted ascending
};

// Round-2 input key: aggregated miss key
// tokenids_unmap holds the *first* tokenid of each miss group
struct Key1 {
    std::string           reqid;
    std::uint32_t         layerid {0};
    std::vector<std::uint32_t> tokenids_unmap;
};

// ---------- round-1 results ----------

// Hit entry: token resolved locally in DRAM
struct HitEntry {
    std::uint32_t tokenid {0};
    void*         dram_addr {nullptr};   // local DRAM source pointer
};

// Miss group:
//   * group covers [first_tokenid, first_tokenid + kGroupSize)
//   * bitmap[i] = 1 iff (first_tokenid + i) is a miss in this group
//   * offsets are differential offsets from first_tokenid (incl. 0 for the first)
struct MissGroup {
    std::uint32_t              first_tokenid {0};
    std::uint16_t              bitmap {0};
    std::vector<std::uint32_t> offsets;       // diff offsets, offsets[0]==0
};

struct FirstQueryResult {
    std::vector<HitEntry>  hits;
    std::vector<MissGroup> miss_groups;
    Key1                   aggregated_key;    // for round-2
};

// ---------- round-2 result ----------

struct SecondQueryEntry {
    std::uint64_t hash_key {0};         // produced by round-2 metadata
    std::string   ip_address;           // physical server holding the data
    std::uint32_t first_tokenid {0};    // for cross-reference w/ MissGroup
};

// ---------- round-3 result ----------

struct ThirdQueryEntry {
    void*         dram_addr {nullptr};  // base addr for this group's first token
    std::uint64_t ssd_lba   {0};        // alt: SSD LBA when not in DRAM
    bool          is_dram   {true};
};

// ---------- low-level transfer engine ----------

class TransferEngine {
public:
    // Single contiguous DRAM->HBM(or any dst) copy.
    static bool transfer_dram_to_hbm(const void* src, void* dst, std::size_t size);

    // Multi-source gather: concatenate srcs[i] (each per_size bytes) into dst.
    // Underlying impl can be a single network gather primitive, or a sequence
    // of independent per-source transfers (here: per-source memcpy).
    static bool gather_transfer(const std::vector<const void*>& srcs,
                                void*                            dst,
                                std::size_t                      per_size);
};

// ---------- Client (rounds 1 & 2) ----------

class Client {
public:
    Client();

    // Round 1: metadata lookup for the first storage space.
    // Stubbed: even-tokenid -> hit, odd-tokenid -> miss (deterministic for tests).
    FirstQueryResult first_query(const Key& key);

    // Issue DRAM->HBM transfers for every hit token.
    // hbm_buffer must be at least hits.size() * kTokenSize bytes.
    bool process_hits(const std::vector<HitEntry>& hits, void* hbm_buffer);

    // Round 2: metadata lookup for the second storage space.
    // Stubbed: returns local ip + a synthetic hash key per first_tokenid.
    std::vector<SecondQueryEntry> second_query(const Key1& key1);

    // Local-vs-remote dispatch.
    static bool is_local_ip(const std::string& ip);

    // Reserved interface for remote transfer (NoF / RDMA two-sided).
    // Not implemented in this drop; returns false.
    bool remote_transfer(const SecondQueryEntry& entry, void* dst);
};

// ---------- Server (round 3) ----------

class Server {
public:
    Server();

    // Round 3: metadata lookup keyed by hash. Stubbed: always DRAM hit,
    // mapped into a fixed mock pool.
    std::vector<ThirdQueryEntry> third_query(const std::vector<std::uint64_t>& hash_keys);

    // Gather + transfer for one batch of (hash_keys, miss_groups).
    // For each group, builds a gather list of (base + offset_i * kTokenSize)
    // and writes contiguously into hbm_buffer.
    // hbm_buffer must be large enough for sum(group.offsets.size()) * kTokenSize.
    bool handle_transfer_request(const std::vector<std::uint64_t>& hash_keys,
                                 const std::vector<MissGroup>&     miss_groups,
                                 void*                              hbm_buffer);
};

}  // namespace kv_transfer
