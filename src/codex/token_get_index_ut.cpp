// token_get_index_ut.cpp -- minimal tests for token_get_index.

#include "kv_transfer.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace kv_transfer;

namespace {

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::fprintf(stderr, "FAIL: %s @ %s:%d\n",                        \
                         #cond, __FILE__, __LINE__);                          \
            std::exit(1);                                                     \
        }                                                                     \
    } while (0)

#define RUN(test_fn)                                       \
    do {                                                   \
        std::cout << "[ RUN  ] " #test_fn << '\n';         \
        test_fn();                                         \
        std::cout << "[  OK  ] " #test_fn << '\n';         \
    } while (0)

void test_token_get_index_typical_key() {
    Key key;
    key.reqid = "req-tgi";
    key.layerid = 3;
    key.tokenids = {0, 1, 2, 3, 4, 5, 6, 7};

    std::vector<unsigned char> hbm(key.tokenids.size() * kTokenSize, 0);
    Kvof kvof(hbm.data(), hbm.size());

    EXPECT(kvof.token_get_index(key));
}

void test_token_get_index_empty_key() {
    Key key;
    key.reqid = "empty";

    std::vector<unsigned char> hbm;
    Kvof kvof(hbm.data(), hbm.size());

    EXPECT(kvof.token_get_index(key));
}

void test_meta_info_variant_shape() {
    MetadataEngine engine;

    Key key;
    key.reqid = "req-shape";
    key.layerid = 9;
    key.tokenids = {10, 11, 12, 13};

    auto first = engine.batchQuery(key);
    EXPECT(first.size() == key.tokenids.size());

    for (const auto& meta : first) {
        EXPECT(meta.kind == MetaInfoKind::kVa ||
               meta.kind == MetaInfoKind::kIpTokenkey);
        if (meta.kind == MetaInfoKind::kIpTokenkey) {
            EXPECT(!meta.ip_tokenkey.ip.empty());
            EXPECT(!meta.ip_tokenkey.tokenkey.empty());
        }
    }

    std::vector<std::string> tokenkeys = {"tk-a", "tk-b"};
    auto second = engine.batchQueryLocal(tokenkeys);
    EXPECT(second.size() == tokenkeys.size());
    for (const auto& meta : second) {
        EXPECT(meta.kind == MetaInfoKind::kVa);
        EXPECT(meta.va.va != nullptr);
    }
}

void test_free_function_wraps_kvof() {
    Key key;
    key.reqid = "req-free";
    key.layerid = 1;
    key.tokenids = {1, 2, 3};

    EXPECT(token_get_index(key));
}

void print_bytes(const char* label, const unsigned char* data, std::size_t count) {
    std::cout << label;
    for (std::size_t i = 0; i < count; ++i) {
        std::cout << " 0x" << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(data[i]);
    }
    std::cout << std::dec << std::setfill(' ') << '\n';
}

void test_va_transfer_copies_data_to_hbm() {
    alignas(64) unsigned char src[kTokenSize];
    std::vector<unsigned char> hbm(kTokenSize * 2, 0);

    for (std::size_t i = 0; i < kTokenSize; ++i) {
        src[i] = static_cast<unsigned char>((0xA0 + i) & 0xFF);
    }

    Kvof kvof(hbm.data(), hbm.size());
    EXPECT(kvof.va_to_hbm(src, 1) == 0);

    const unsigned char* copied = hbm.data() + kTokenSize;
    print_bytes("src[0..15] =", src, 16);
    print_bytes("hbm[slot1][0..15] =", copied, 16);

    EXPECT(std::memcmp(src, copied, kTokenSize) == 0);
    for (std::size_t i = 0; i < kTokenSize; ++i) {
        EXPECT(hbm[i] == 0u);
    }
}

}  // namespace

int main() {
    RUN(test_token_get_index_typical_key);
    RUN(test_token_get_index_empty_key);
    RUN(test_meta_info_variant_shape);
    RUN(test_free_function_wraps_kvof);
    RUN(test_va_transfer_copies_data_to_hbm);
    std::cout << "\nAll token_get_index tests passed.\n";
    return 0;
}
