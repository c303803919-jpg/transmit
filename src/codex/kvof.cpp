// kvof.cpp -- Kvof token_get_index orchestration.

#include "kv_transfer.h"

#include <iostream>
#include <string>
#include <vector>

namespace kv_transfer {

namespace {

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

}  // namespace kv_transfer
