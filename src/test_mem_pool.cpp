// test_mem_pool.cpp
// MemPool 基础 UT
// 编译: g++ -std=c++17 -O2 -pthread test_mem_pool.cpp -o test_mem_pool

#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <queue>
#include <set>
#include <vector>

// ---- 被测类（与 mem_pool.cpp 中 class 一致，直接内嵌省得拆头文件）----
class MemPool {
public:
    MemPool() = default;
    ~MemPool() = default;
    MemPool(const MemPool&) = delete;
    MemPool& operator=(const MemPool&) = delete;

    bool memInit(size_t unitSize, size_t unitCount) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (initialized_ || unitSize == 0 || unitCount == 0) return false;
        try { buffer_.resize(unitSize * unitCount); }
        catch (const std::bad_alloc&) { return false; }
        unitSize_ = unitSize; totalUnits_ = unitCount;
        for (size_t i = 0; i < unitCount; ++i)
            freeList_.push(buffer_.data() + i * unitSize);
        initialized_ = true;
        return true;
    }
    std::vector<void*> memAlloc(size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<void*> r;
        if (!initialized_ || count == 0 || count > freeList_.size()) return r;
        r.reserve(count);
        for (size_t i = 0; i < count; ++i) { r.push_back(freeList_.front()); freeList_.pop(); }
        return r;
    }
    void* memAllocOne() { auto v = memAlloc(1); return v.empty() ? nullptr : v.front(); }
    void memFree(void* ptr) {
        if (!ptr) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isFromPool(ptr)) return;
        freeList_.push(ptr);
    }
    void memFree(const std::vector<void*>& ptrs) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto* p : ptrs) if (p && isFromPool(p)) freeList_.push(p);
    }
    size_t unitSize()   const { return unitSize_; }
    size_t totalUnits() const { return totalUnits_; }
    size_t availableUnits() { std::lock_guard<std::mutex> lock(mutex_); return freeList_.size(); }
    void*  basePtr()          { return buffer_.empty() ? nullptr : buffer_.data(); }
    size_t bufferBytes() const { return buffer_.size(); }
private:
    bool isFromPool(void* ptr) const {
        auto* base = const_cast<uint8_t*>(buffer_.data());
        return ptr >= base && ptr < base + buffer_.size();
    }
    std::vector<uint8_t> buffer_;
    std::queue<void*>    freeList_;
    size_t unitSize_ = 0, totalUnits_ = 0;
    std::mutex mutex_;
    bool initialized_ = false;
};

// ====== 极简测试框架 ======
static int  g_total = 0, g_pass = 0, g_fail = 0;
static bool g_case_failed = false;
static const char* g_case_name = "";

#define CASE(name) do { \
    g_total++; g_case_failed = false; g_case_name = name; \
    std::cout << "[ RUN  ] " << name << "\n"; \
} while(0)

#define END_CASE() do { \
    if (g_case_failed) { g_fail++; std::cout << "\033[31m[ FAIL ]\033[0m " << g_case_name << "\n\n"; } \
    else               { g_pass++; std::cout << "\033[32m[ PASS ]\033[0m " << g_case_name << "\n\n"; } \
} while(0)

#define CHECK(cond) do { \
    if (!(cond)) { g_case_failed = true; \
        std::cout << "  x CHECK failed: " #cond " @ line " << __LINE__ << "\n"; } \
} while(0)


// ====== 测试用例 ======
static void test_init() {
    CASE("TC01 memInit 正常初始化");
    MemPool p;
    CHECK(p.memInit(4096, 16));
    CHECK(p.unitSize()       == 4096u);
    CHECK(p.totalUnits()     == 16u);
    CHECK(p.availableUnits() == 16u);
    CHECK(p.basePtr() != nullptr);
    END_CASE();
}

static void test_init_invalid() {
    CASE("TC02 memInit 非法参数应失败");
    MemPool p;
    CHECK(!p.memInit(0, 16));       // unitSize = 0
    CHECK(!p.memInit(4096, 0));     // unitCount = 0
    MemPool q;
    CHECK(q.memInit(64, 4));
    CHECK(!q.memInit(64, 4));       // 重复初始化
    END_CASE();
}

static void test_alloc_free() {
    CASE("TC03 单个 alloc/free 往返");
    MemPool p;
    CHECK(p.memInit(128, 8));
    void* x = p.memAllocOne();
    CHECK(x != nullptr);
    CHECK(p.availableUnits() == 7u);
    p.memFree(x);
    CHECK(p.availableUnits() == 8u);
    END_CASE();
}

static void test_alloc_batch() {
    CASE("TC04 批量 alloc/free");
    MemPool p;
    CHECK(p.memInit(128, 16));
    auto v = p.memAlloc(10);
    CHECK(v.size() == 10u);
    CHECK(p.availableUnits() == 6u);
    p.memFree(v);
    CHECK(p.availableUnits() == 16u);
    END_CASE();
}

static void test_exhaust() {
    CASE("TC05 耗尽池后申请失败，超额申请是原子的");
    MemPool p;
    CHECK(p.memInit(64, 4));
    auto v = p.memAlloc(4);
    CHECK(v.size() == 4u);
    CHECK(p.availableUnits() == 0u);
    CHECK(p.memAllocOne() == nullptr);
    p.memFree(v);
    CHECK(p.memAlloc(5).empty());   // 超额申请不应扣状态
    CHECK(p.availableUnits() == 4u);
    END_CASE();
}

static void test_free_invalid() {
    CASE("TC06 非法 free 安全处理");
    MemPool p;
    CHECK(p.memInit(64, 4));
    p.memFree(nullptr);             // 空指针
    int stack_var = 0;
    p.memFree(&stack_var);          // 外部指针
    CHECK(p.availableUnits() == 4u);
    END_CASE();
}

static void test_unique_pointers() {
    CASE("TC07 分配指针互不重复，且在池内对齐");
    MemPool p;
    const size_t US = 128, N = 16;
    CHECK(p.memInit(US, N));
    auto v = p.memAlloc(N);
    CHECK(v.size() == N);
    std::set<void*> uniq(v.begin(), v.end());
    CHECK(uniq.size() == N);
    auto* base = static_cast<uint8_t*>(p.basePtr());
    for (auto* ptr : v) {
        auto* u = static_cast<uint8_t*>(ptr);
        CHECK(u >= base && u < base + US * N);
        CHECK(static_cast<size_t>(u - base) % US == 0u);
    }
    END_CASE();
}

static void test_memory_rw() {
    CASE("TC08 各单元读写隔离，无串扰");
    MemPool p;
    const size_t US = 256, N = 8;
    CHECK(p.memInit(US, N));
    auto v = p.memAlloc(N);
    for (size_t i = 0; i < v.size(); ++i)
        std::memset(v[i], static_cast<int>(i & 0xFF), US);
    for (size_t i = 0; i < v.size(); ++i) {
        auto* b = static_cast<uint8_t*>(v[i]);
        for (size_t k = 0; k < US; ++k)
            CHECK(b[k] == static_cast<uint8_t>(i & 0xFF));
    }
    END_CASE();
}


int main() {
    std::cout << "======== MemPool UT ========\n\n";
    test_init();
    test_init_invalid();
    test_alloc_free();
    test_alloc_batch();
    test_exhaust();
    test_free_invalid();
    test_unique_pointers();
    test_memory_rw();

    std::cout << "======== Summary ========\n";
    std::cout << "Total : " << g_total << "\n";
    std::cout << "\033[32mPassed: " << g_pass << "\033[0m\n";
    std::cout << "\033[31mFailed: " << g_fail << "\033[0m\n";
    std::cout << (g_fail == 0 ? "\033[32mResult: ALL PASSED\033[0m\n"
                              : "\033[31mResult: FAILED\033[0m\n");
    return g_fail == 0 ? 0 : 1;
}
