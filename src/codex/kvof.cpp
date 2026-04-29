// kvof.cpp -- Kvof token_get_index orchestration.

#include "kv_transfer.h"

#include <iostream>
#include <string>
#include <vector>

namespace kv_transfer {

namespace {

bool transfer(Kvof& kvof, const MetaInfo& meta, std::size_t slot_index) {
    if (const auto* va = std::get_if<VaMeta>(&meta)) {
        return kvof.va_to_hbm(va->va, slot_index) == 0;
    }
    if (const auto* lba = std::get_if<LbaMeta>(&meta)) {
        return kvof.lba_to_hbm(lba->lba, slot_index) == 0;
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
        if (const auto* remote = std::get_if<IpTokenkeyMeta>(&meta)) {
            remote_tokenkeys.push_back(remote->tokenkey);
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
