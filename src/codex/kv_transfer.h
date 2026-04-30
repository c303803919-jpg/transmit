// kv_transfer.h
// Minimal token_get_index metadata-query + transfer skeleton.

#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace kv_transfer {

constexpr std::size_t kTokenSize = 576;

struct Key {
    std::string reqid;
    std::uint32_t layerid {0};
    std::vector<std::uint32_t> tokenids;
};

struct VaMeta {
    void* va {nullptr};
};

struct LbaMeta {
    std::uint64_t lba {0};
};

struct IpTokenkeyMeta {
    std::string ip;
    std::string tokenkey;
};

enum class MetaInfoKind : std::uint32_t {
    kVa = 0,
    kLba,
    kIpTokenkey,
};

struct MetaInfo {
    MetaInfoKind kind {MetaInfoKind::kVa};
    VaMeta va;
    LbaMeta lba;
    IpTokenkeyMeta ip_tokenkey;

    static MetaInfo from_va(void* addr) {
        MetaInfo meta;
        meta.kind = MetaInfoKind::kVa;
        meta.va.va = addr;
        return meta;
    }

    static MetaInfo from_lba(std::uint64_t value) {
        MetaInfo meta;
        meta.kind = MetaInfoKind::kLba;
        meta.lba.lba = value;
        return meta;
    }

    static MetaInfo from_ip_tokenkey(const std::string& ip,
                                     const std::string& tokenkey) {
        MetaInfo meta;
        meta.kind = MetaInfoKind::kIpTokenkey;
        meta.ip_tokenkey.ip = ip;
        meta.ip_tokenkey.tokenkey = tokenkey;
        return meta;
    }
};

class MetadataEngine {
public:
    MetadataEngine();

    std::vector<MetaInfo> batchQuery(const Key& key);
    std::vector<MetaInfo> batchQueryLocal(const std::vector<std::string>& tokenkeys);

private:
    std::mt19937_64 rng_ {0xC0FFEEULL};
};

class Kvof {
public:
    Kvof();
    Kvof(void* hbm, std::size_t hbm_capacity);

    bool token_get_index(const Key& key);

    int va_to_hbm(const void* va, std::size_t slot_index);
    int lba_to_hbm(std::uint64_t lba, std::size_t slot_index);

private:
    MetadataEngine metadata_;
    void* hbm_ {nullptr};
    std::size_t hbm_capacity_ {0};
};

bool token_get_index(const Key& key);

// C-style bridge used by NPU->CPU request path.
// Preconditions:
//   * req_lst points to batch_size C strings.
//   * layerid_lst points to batch_size int32 values.
//   * index_lst points to (batch_size * num_segment) int32 values
//     in row-major layout.
// Returns a non-zero kvof_ID on success, or 0 on input/processing error.
std::uint64_t kvof_get(const char* const* req_lst,
                       const std::int32_t* layerid_lst,
                       const std::int32_t* index_lst,
                       int batch_size,
                       int segment_size,
                       int num_segment,
                       int src_media_type,
                       int dst_media_type);

}  // namespace kv_transfer
