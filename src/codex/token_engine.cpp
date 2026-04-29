// token_engine.cpp -- minimal Engine + token_get_index implementation.

#include "kv_transfer.h"

#include <cstddef>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <utility>

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

constexpr std::size_t kClientDramTokens = 4096;
constexpr std::size_t kServerDramTokens = 8192;
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

}  // namespace

MetadataEngine::MetadataEngine() = default;

std::vector<MetaInfo> MetadataEngine::batchQuery(const Key& key) {
    std::vector<MetaInfo> out;
    out.reserve(key.tokenids.size());

    for (std::uint32_t tokenid : key.tokenids) {
        if ((rng_() & 1ULL) == 0ULL) {
            out.push_back(MetaInfo::from_va(client_va(tokenid)));
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
        out.push_back(MetaInfo::from_va(server_va(tokenkey)));
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
    // Reserved: SSD LBA -> HBM will issue SSD read plus aclrtMemcpy later.
    return 0;
}

}  // namespace kv_transfer
