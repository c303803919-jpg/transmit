// token_get_index.cpp -- minimal token_get_index path.

#include "kv_transfer.h"

#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

namespace kv_transfer {

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
        if (meta.kind == MetaInfoKind::kVa) {
            if (va_to_hbm(meta.va.va, i) != 0) {
                return false;
            }
            continue;
        }
        if (meta.kind == MetaInfoKind::kLba) {
            if (lba_to_hbm(meta.lba.lba, i) != 0) {
                return false;
            }
            continue;
        }
        return false;
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
        const auto& meta = second[i];
        if (meta.kind == MetaInfoKind::kVa) {
            if (va_to_hbm(meta.va.va, remote_indices[i]) != 0) {
                return false;
            }
            continue;
        }
        if (meta.kind == MetaInfoKind::kLba) {
            if (lba_to_hbm(meta.lba.lba, remote_indices[i]) != 0) {
                return false;
            }
            continue;
        }
        return false;
    }

    return true;
}

bool token_get_index(const Key& key) {
    std::vector<unsigned char> hbm(key.tokenids.size() * kTokenSize, 0);
    Kvof kvof(hbm.data(), hbm.size());
    return kvof.token_get_index(key);
}

}  // namespace kv_transfer