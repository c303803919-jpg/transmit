#include "kvof_get_cpu_handler.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

using kv_transfer::kvof_npu::RequestBlock;
using namespace kv_transfer::kvof_npu;

namespace {

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::fprintf(stderr, "FAIL: %s @ %s:%d\n", #cond, __FILE__, __LINE__); \
            std::exit(1);                                                     \
        }                                                                     \
    } while (0)

#define RUN(test_fn)                                       \
    do {                                                   \
        std::cout << "[ RUN  ] " #test_fn << '\n';         \
        test_fn();                                         \
        std::cout << "[  OK  ] " #test_fn << '\n';         \
    } while (0)

RequestBlock make_valid_request() {
    RequestBlock req {};
    req.flag = kFlagRequest;
    req.batch_size = 2;
    req.segment_size = 1;
    req.num_segment = 3;
    req.src_media_type = 0;
    req.dst_media_type = 1;

    std::strcpy(req.req_lst[0], "cpu-ut-req-0");
    std::strcpy(req.req_lst[1], "cpu-ut-req-1");
    req.layerid_lst[0] = 7;
    req.layerid_lst[1] = 8;

    req.index_lst[0][0] = 101;
    req.index_lst[0][1] = 102;
    req.index_lst[0][2] = 103;
    req.index_lst[1][0] = 201;
    req.index_lst[1][1] = 202;
    req.index_lst[1][2] = 203;

    return req;
}

void test_process_kvof_get_request_success() {
    auto req = make_valid_request();
    EXPECT(process_kvof_get_request(&req));
    EXPECT(req.flag == kFlagResponse);
    EXPECT(req.kvof_id != 0);
}

void test_process_kvof_get_request_bad_shape() {
    auto req = make_valid_request();
    req.batch_size = 0;
    EXPECT(!process_kvof_get_request(&req));
    EXPECT(req.flag == kFlagError);

    req = make_valid_request();
    req.num_segment = kMaxNumSegment + 1;
    EXPECT(!process_kvof_get_request(&req));
    EXPECT(req.flag == kFlagError);
}

void test_process_kvof_get_request_bad_req_cstr() {
    auto req = make_valid_request();
    for (std::uint32_t i = 0; i < kMaxReqLen; ++i) {
        req.req_lst[0][i] = 'x';
    }

    EXPECT(!process_kvof_get_request(&req));
    EXPECT(req.flag == kFlagError);
}

void test_process_kvof_get_request_requires_request_flag() {
    auto req = make_valid_request();
    req.flag = kFlagIdle;
    EXPECT(!process_kvof_get_request(&req));
}

// Verify that successive calls return strictly increasing kvof_ids.
// This confirms the handler correctly writes kvof_get()'s return value
// into req->kvof_id and that the underlying ID counter is live.
void test_kvof_id_increments() {
    auto req0 = make_valid_request();
    auto req1 = make_valid_request();

    EXPECT(process_kvof_get_request(&req0));
    EXPECT(process_kvof_get_request(&req1));

    EXPECT(req0.flag == kFlagResponse);
    EXPECT(req1.flag == kFlagResponse);
    EXPECT(req0.kvof_id != 0);
    EXPECT(req1.kvof_id != 0);
    EXPECT(req1.kvof_id > req0.kvof_id);
}

// Verify that the handler handles the maximum allowed batch_size and
// num_segment without out-of-bounds access or spurious errors.
void test_max_batch_and_segment() {
    RequestBlock req {};
    req.flag = kFlagRequest;
    req.batch_size = kMaxBatchSize;
    req.segment_size = 1;
    req.num_segment = kMaxNumSegment;
    req.src_media_type = 0;
    req.dst_media_type = 1;

    for (std::uint32_t b = 0; b < kMaxBatchSize; ++b) {
        // Write a short, null-terminated string that fits in kMaxReqLen.
        req.req_lst[b][0] = 'r';
        req.req_lst[b][1] = static_cast<char>('0' + b);
        req.req_lst[b][2] = '\0';
        req.layerid_lst[b] = static_cast<std::int32_t>(b);
        for (std::uint32_t s = 0; s < kMaxNumSegment; ++s) {
            req.index_lst[b][s] = static_cast<std::int32_t>(b * kMaxNumSegment + s);
        }
    }

    EXPECT(process_kvof_get_request(&req));
    EXPECT(req.flag == kFlagResponse);
    EXPECT(req.kvof_id != 0);
}

}  // namespace

int main() {
    RUN(test_process_kvof_get_request_success);
    RUN(test_process_kvof_get_request_bad_shape);
    RUN(test_process_kvof_get_request_bad_req_cstr);
    RUN(test_process_kvof_get_request_requires_request_flag);
    RUN(test_kvof_id_increments);
    RUN(test_max_batch_and_segment);
    std::cout << "\nAll kvof_get_cpu_handler tests passed.\n";
    return 0;
}
