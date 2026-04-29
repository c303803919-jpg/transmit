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
#include <functional>
#include <random>
#include <string>
#include <vector>

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

// ============================================================================
// New API: Engine + token_get_index
// ----------------------------------------------------------------------------
// High-level flow that replaces the multi-round Client/Server interface above
// for the NPU-driven KV fetch path. There is exactly one client-side query
// (Engine::batchQuery) and at most one server-side query (Engine::batchQueryLocal).
// The two queries return MetaInfo entries; token_get_index inspects each
// entry and dispatches to the right transfer primitive:
//
//   batchQuery(key) -> vector<MetaInfo>            (random VA / IP+tokenkey)
//      | per entry:
//      v
//   kVa         -> va_to_hbm(va, hbm, sz)              (empty stub: returns 0)
//   kLba        -> lba_to_hbm(lba, hbm, sz)            (reserved interface)
//   kIpTokenkey -> defer; collect tokenkeys
//
//   batchQueryLocal(tokenkeys) -> vector<MetaInfo>     (always VA in stub)
//      | per entry:
//      v
//   kVa  -> va_to_hbm(...)
//   kLba -> lba_to_hbm(...)                            (reserved interface)
// ============================================================================

// Discriminator for MetaInfo. Tells token_get_index which transfer to invoke.
enum class MetaInfoType : std::uint32_t {
    kInvalid    = 0,
    kVa         = 1,   // local DRAM VA -- read memory to HBM
    kLba        = 2,   // SSD LBA       -- read SSD to HBM (reserved interface)
    kIpTokenkey = 3,   // remote        -- dispatch via batchQueryLocal
};

// Result of a single-token metadata lookup. Only the fields relevant to
// `type` carry meaningful values; the rest stay default.
struct MetaInfo {
    MetaInfoType  type     {MetaInfoType::kInvalid};
    std::uint32_t tokenid  {0};
    void*         va       {nullptr};   // valid when type == kVa
    std::uint64_t lba      {0};         // valid when type == kLba
    std::string   ip;                   // valid when type == kIpTokenkey
    std::uint64_t tokenkey {0};         // valid when type == kIpTokenkey
                                        // (also echoed by batchQueryLocal)
};

// Engine -- exposes the two metadata queries, the two transfer primitives,
// and the high-level token_get_index entry point.
class Engine {
public:
    // Stub-mode for batchQuery. Default is random; tests pin to a deterministic
    // mode so transfer counts can be asserted exactly.
    enum class QueryStubMode : std::uint32_t {
        kRandom      = 0,   // 50/50 VA vs IP per token (uses internal RNG)
        kAlwaysVa    = 1,
        kAlwaysIp    = 2,
        kAlwaysLba   = 3,   // emit kLba instead of kVa (exercises LBA path)
        kAlternating = 4,   // even idx -> VA, odd idx -> IP (deterministic)
    };

    Engine();
    ~Engine() = default;

    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&)                 = default;
    Engine& operator=(Engine&&)      = default;

    // ---------- top-level ----------
    // Triggered by the NPU read request. Runs:
    //   1) one batchQuery,
    //   2) per-entry transfer dispatch (VA / LBA inline; IP+tokenkey deferred),
    //   3) at most one batchQueryLocal for the deferred set,
    //   4) per-entry transfer dispatch for the round-2 results.
    // Returns true iff every step succeeded.
    bool token_get_index(const Key& key);

    // ---------- metadata queries ----------
    // Client side. One call covers the whole input key.
    std::vector<MetaInfo> batchQuery(const Key& key);

    // Server side. One call covers all deferred tokenkeys.
    std::vector<MetaInfo>
    batchQueryLocal(const std::vector<std::uint64_t>& tokenkeys);

    // ---------- transfer primitives ----------
    // VA -> HBM. Empty stub: returns 0 (success).
    int va_to_hbm(const void* va, void* hbm, std::size_t size);
    // LBA -> HBM. Reserved interface; returns 0 today.
    int lba_to_hbm(std::uint64_t lba, void* hbm, std::size_t size);

    // ---------- configuration ----------
    void set_query_stub(QueryStubMode mode);
    void set_seed(std::uint64_t seed);
    void set_token_size(std::size_t token_size);
    // Optional. Transfers don't actually copy bytes today, so this is only
    // used so that the dispatched HBM slot pointers are real (non-null) if
    // a future implementation needs them.
    void set_hbm(void* hbm_buf, std::size_t hbm_capacity);

    // ---------- test introspection ----------
    std::size_t batch_query_count()       const { return cnt_batch_query_;       }
    std::size_t batch_query_local_count() const { return cnt_batch_query_local_; }
    std::size_t va_to_hbm_count()         const { return cnt_va_to_hbm_;         }
    std::size_t lba_to_hbm_count()        const { return cnt_lba_to_hbm_;        }
    void        reset_counters();

private:
    QueryStubMode   stub_mode_     {QueryStubMode::kRandom};
    std::mt19937_64 rng_           {0xC0FFEEULL};
    std::size_t     token_size_    {kTokenSize};
    void*           hbm_buf_       {nullptr};
    std::size_t     hbm_capacity_  {0};

    std::size_t cnt_batch_query_       {0};
    std::size_t cnt_batch_query_local_ {0};
    std::size_t cnt_va_to_hbm_         {0};
    std::size_t cnt_lba_to_hbm_        {0};
};

// Free-function form -- the canonical signature requested by callers.
// Internally constructs a default Engine and calls token_get_index on it.
bool token_get_index(const Key& key);

// Test-friendly overload -- runs the same flow against a caller-provided
// Engine so its stub mode and counters can be inspected.
bool token_get_index(const Key& key, Engine& engine);

}  // namespace kv_transfer
