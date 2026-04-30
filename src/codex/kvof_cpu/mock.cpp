// mock.cpp -- mock metadata, transfer, and kvof_get bridge.

#include "kv_transfer.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

#ifdef KVOF_USE_ACL
#include "acl/acl.h"
#define KVOF_HAS_ACL 1
#endif

#ifndef KVOF_HAS_ACL
using aclError = int;
constexpr aclError ACL_SUCCESS = 0;
constexpr int ACL_MEMCPY_HOST_TO_DEVICE = 1;
aclError aclrtMemcpy(void* dst, std::size_t destMax, const void* src,
                     std::size_t count, int /*kind*/) {
    if (dst == nullptr || src == nullptr || count > destMax) {
        return -1;
    }
    std::memcpy(dst, src, count);
    return ACL_SUCCESS;
}
#endif

namespace kv_transfer {

namespace {

std::atomic<std::uint64_t> g_kvof_next_id {1};
constexpr std::size_t kClientDramTokens = 4096;
constexpr std::size_t kServerDramTokens = 8192;
constexpr std::size_t kMaxReqIdBytes = 4096;
constexpr const char* kRemoteIp = "10.0.0.42";

char g_client_dram[kClientDramTokens * kTokenSize];
char g_server_dram[kServerDramTokens * kTokenSize];

void* client_va(std::uint32_t tokenid) {
    return g_client_dram + (tokenid % kClientDramTokens) * kTokenSize;
}

void* server_va(const std::string& tokenkey) {
    std::size_t hash = std::hash<std::string>{}(tokenkey);
    return g_server_dram + (hash % kServerDramTokens) * kTokenSize;
}

std::string make_tokenkey(const Key& key, std::uint32_t tokenid) {
    return key.reqid + ":" + std::to_string(key.layerid) + ":" +
           std::to_string(tokenid);
}

std::uint32_t tokenid_from_tokenkey(const std::string& tokenkey) {
    const std::size_t pos = tokenkey.rfind(':');
    if (pos == std::string::npos || pos + 1 >= tokenkey.size()) {
        return 0;
    }
    return static_cast<std::uint32_t>(
        std::strtoul(tokenkey.c_str() + pos + 1, nullptr, 10));
}

void stamp_token(void* token, std::uint32_t tokenid) {
    std::memset(token, static_cast<int>(tokenid & 0xFF), kTokenSize);
}

}  // namespace

MetadataEngine::MetadataEngine() = default;

std::vector<MetaInfo> MetadataEngine::batchQuery(const Key& key) {
    std::vector<MetaInfo> out;
    out.reserve(key.tokenids.size());

    for (std::uint32_t tokenid : key.tokenids) {
        if ((rng_() & 1ULL) == 0ULL) {
            void* va = client_va(tokenid);
            stamp_token(va, tokenid);
            out.push_back(MetaInfo::from_va(va));
        } else {
            out.push_back(MetaInfo::from_ip_tokenkey(
                kRemoteIp, make_tokenkey(key, tokenid)));
        }
    }

    return out;
}

std::vector<MetaInfo>
MetadataEngine::batchQueryLocal(const std::vector<std::string>& tokenkeys) {
    std::vector<MetaInfo> out;
    out.reserve(tokenkeys.size());

    for (const auto& tokenkey : tokenkeys) {
        void* va = server_va(tokenkey);
        stamp_token(va, tokenid_from_tokenkey(tokenkey));
        out.push_back(MetaInfo::from_va(va));
    }

    return out;
}

Kvof::Kvof() = default;

Kvof::Kvof(void* hbm, std::size_t hbm_capacity)
    : hbm_(hbm), hbm_capacity_(hbm_capacity) {}

int Kvof::va_to_hbm(const void* va, std::size_t slot_index) {
    if (va == nullptr || hbm_ == nullptr) {
        return -1;
    }

    const std::size_t offset = slot_index * kTokenSize;
    if (offset > hbm_capacity_ || kTokenSize > hbm_capacity_ - offset) {
        return -1;
    }

    void* dst = static_cast<char*>(hbm_) + offset;
    const aclError ret = aclrtMemcpy(dst, kTokenSize, va, kTokenSize,
                                     ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != ACL_SUCCESS) {
        std::cerr << "aclrtMemcpy host-to-HBM failed, ret=" << ret << '\n';
        return -1;
    }
    return 0;
}

int Kvof::lba_to_hbm(std::uint64_t /*lba*/, std::size_t /*slot_index*/) {
    return 0;
}

std::uint64_t kvof_get(const char* const* req_lst,
                       const std::int32_t* layerid_lst,
                       const std::int32_t* index_lst,
                       int batch_size,
                       int segment_size,
                       int num_segment,
                       int src_media_type,
                       int dst_media_type) {
    (void)segment_size;
    (void)src_media_type;
    (void)dst_media_type;

    if (batch_size < 0 || num_segment < 0) {
        return 0;
    }

    if (batch_size == 0) {
        return g_kvof_next_id.fetch_add(1, std::memory_order_relaxed);
    }

    if (req_lst == nullptr || layerid_lst == nullptr || index_lst == nullptr) {
        return 0;
    }

    const std::size_t token_count = static_cast<std::size_t>(num_segment);
    if (token_count != 0 &&
        static_cast<std::size_t>(batch_size) >
            (std::numeric_limits<std::size_t>::max() / token_count)) {
        return 0;
    }
    const std::size_t total_tokens =
        static_cast<std::size_t>(batch_size) * token_count;
    if (total_tokens != 0 &&
        total_tokens > (std::numeric_limits<std::size_t>::max() / kTokenSize)) {
        return 0;
    }

    try {
        std::vector<Key> keys;
        keys.reserve(static_cast<std::size_t>(batch_size));

        for (int i = 0; i < batch_size; ++i) {
            if (layerid_lst[i] < 0 || req_lst[i] == nullptr) {
                return 0;
            }

            const std::size_t req_len = ::strnlen(req_lst[i], kMaxReqIdBytes);
            if (req_len == kMaxReqIdBytes) {
                return 0;
            }

            Key key;
            key.reqid.assign(req_lst[i], req_len);
            key.layerid = static_cast<std::uint32_t>(layerid_lst[i]);
            key.tokenids.reserve(token_count);

            const std::size_t row = static_cast<std::size_t>(i) * token_count;
            for (std::size_t j = 0; j < token_count; ++j) {
                const std::int32_t idx = index_lst[row + j];
                if (idx < 0) {
                    return 0;
                }
                key.tokenids.push_back(static_cast<std::uint32_t>(idx));
            }

            keys.push_back(std::move(key));
        }

        std::vector<unsigned char> hbm(total_tokens * kTokenSize, 0);
        Kvof kvof(hbm.data(), hbm.size());

        for (const auto& key : keys) {
            if (!kvof.token_get_index(key)) {
                return 0;
            }
        }
    } catch (const std::bad_alloc&) {
        return 0;
    }

    return g_kvof_next_id.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace kv_transfer