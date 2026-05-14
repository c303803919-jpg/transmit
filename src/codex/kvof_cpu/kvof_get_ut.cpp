// kvof_get_ut.cpp
// 最简单的 UT：mock 一个 token_get_index，验证 kvof_get 把 NPU 传过来的
// 三个裸 buffer 正确反序列化成 vector，并按 batch 调到 token_get_index。

#include "kvof_get.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace kvof {

// ---- 内联 mock：记录每次调用的参数 ----
static std::vector<TokenGetIndexRequest> g_mock_calls;

int token_get_index(const TokenGetIndexRequest& req) {
    g_mock_calls.push_back(req);
    return 0;  // 总是成功
}

}  // namespace kvof

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::fprintf(stderr, "FAIL: %s @ %s:%d\n", #cond,                \
                         __FILE__, __LINE__);                                 \
            std::exit(1);                                                     \
        }                                                                     \
    } while (0)

int main() {
    using kvof::g_mock_calls;
    g_mock_calls.clear();

    // 模拟 NPU 写到 device buffer 的三段数据
    const char* req_lst[] = {"req-A", "req-B"};
    const std::int32_t layerid_lst[] = {1, 2};
    const std::int32_t index_lst[] = {
        10, 11, 12,   // req-A 的 num_segment 个 index
        20, 21, 22,   // req-B 的 num_segment 个 index
    };

    const std::uint64_t kvof_id = kvof::kvof_get(
        req_lst, layerid_lst, index_lst,
        /*batch_size=*/2,
        /*segment_size=*/64,
        /*num_segment=*/3,
        /*src_media_type=*/0,
        /*dst_media_type=*/1);

    // 1) 返回了非零 kvof_ID
    EXPECT(kvof_id != 0);

    // 2) batch_size 次调用
    EXPECT(g_mock_calls.size() == 2);

    // 3) 每次调用的字段都正确
    EXPECT(g_mock_calls[0].reqid          == "req-A");
    EXPECT(g_mock_calls[0].layerid        == 1);
    EXPECT(g_mock_calls[0].indices        == (std::vector<std::int32_t>{10, 11, 12}));
    EXPECT(g_mock_calls[0].segment_size   == 64);
    EXPECT(g_mock_calls[0].src_media_type == 0);
    EXPECT(g_mock_calls[0].dst_media_type == 1);

    EXPECT(g_mock_calls[1].reqid          == "req-B");
    EXPECT(g_mock_calls[1].layerid        == 2);
    EXPECT(g_mock_calls[1].indices        == (std::vector<std::int32_t>{20, 21, 22}));

    std::cout << "[ OK ] kvof_get happy-path: kvof_id=" << kvof_id
              << ", calls=" << g_mock_calls.size() << "\n";
    return 0;
}
