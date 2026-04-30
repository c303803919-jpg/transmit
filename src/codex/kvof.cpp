// kvof.cpp -- Kvof token_get_index orchestration.

#include "kv_transfer.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace kv_transfer {

namespace {

std::atomic<std::uint64_t> g_kvof_next_id {1};
constexpr std::size_t kMaxReqIdBytes = 4096;

bool transfer(Kvof& kvof, const MetaInfo& meta, std::size_t slot_index) {
    if (meta.kind == MetaInfoKind::kVa) {
        return kvof.va_to_hbm(meta.va.va, slot_index) == 0;
    }
    if (meta.kind == MetaInfoKind::kLba) {
        return kvof.lba_to_hbm(meta.lba.lba, slot_index) == 0;
    }
    return false;
}

}  // namespace

bool Kvof::token_get_index(const Key& key) {
    const auto first = metadata_.batchQuery(key);
    if (first.size() != key.tokenids.size()) {
        std::cerr << "batchQuery result size mismatch\n";
        return false;
    }

    std::vector<std::string> remote_tokenkeys;
    std::vector<std::size_t> remote_indices;
    remote_tokenkeys.reserve(first.size());
    remote_indices.reserve(first.size());

    for (std::size_t i = 0; i < first.size(); ++i) {
        const auto& meta = first[i];
        if (meta.kind == MetaInfoKind::kIpTokenkey) {
            remote_tokenkeys.push_back(meta.ip_tokenkey.tokenkey);
            remote_indices.push_back(i);
            continue;
        }
        if (!transfer(*this, meta, i)) {
            return false;
        }
    }

    if (remote_tokenkeys.empty()) {
        return true;
    }

    const auto second = metadata_.batchQueryLocal(remote_tokenkeys);
    if (second.size() != remote_tokenkeys.size()) {
        std::cerr << "batchQueryLocal result size mismatch\n";
        return false;
    }

    for (std::size_t i = 0; i < second.size(); ++i) {
        if (!transfer(*this, second[i], remote_indices[i])) {
            return false;
        }
    }

    return true;
}

bool token_get_index(const Key& key) {
    std::vector<unsigned char> hbm(key.tokenids.size() * kTokenSize, 0);
    Kvof kvof(hbm.data(), hbm.size());
    return kvof.token_get_index(key);
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
