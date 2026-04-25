// mem_pool.cpp
// 固定大小数据单元的内存池，适用于传输缓存场景
// 编译：g++ -std=c++17 -O2 -pthread mem_pool.cpp -o mem_pool

#include <vector>
#include <queue>
#include <mutex>
#include <cstdint>
#include <cstddef>
#include <iostream>
#include <cstring>

class MemPool {
public:
    MemPool() = default;
    ~MemPool() = default;

    // 禁用拷贝，避免 buffer 被意外复制
    MemPool(const MemPool&) = delete;
    MemPool& operator=(const MemPool&) = delete;

    // 初始化：预分配 unitCount 个 unitSize 字节的单元
    // 如需注册给网卡/GPU，可在此处 resize 之后调用 ibv_reg_mr / cudaHostRegister
    bool memInit(size_t unitSize, size_t unitCount) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (initialized_ || unitSize == 0 || unitCount == 0) {
            return false;
        }
        try {
            buffer_.resize(unitSize * unitCount);
        } catch (const std::bad_alloc&) {
            return false;
        }

        unitSize_   = unitSize;
        totalUnits_ = unitCount;

        // 把每个单元的起始地址压入空闲队列
        for (size_t i = 0; i < unitCount; ++i) {
            freeList_.push(buffer_.data() + i * unitSize);
        }
        initialized_ = true;
        return true;
    }

    // 申请 count 个内存单元，返回指针数组（不足则返回空）
    std::vector<void*> memAlloc(size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<void*> result;
        if (!initialized_ || count == 0 || count > freeList_.size()) {
            return result;
        }
        result.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            result.push_back(freeList_.front());
            freeList_.pop();
        }
        return result;
    }

    // 申请单个单元的便捷接口
    void* memAllocOne() {
        auto v = memAlloc(1);
        return v.empty() ? nullptr : v.front();
    }

    // 归还一个单元
    void memFree(void* ptr) {
        if (!ptr) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isFromPool(ptr)) return;   // 防止误归还外部指针
        freeList_.push(ptr);
    }

    // 批量归还
    void memFree(const std::vector<void*>& ptrs) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto* p : ptrs) {
            if (p && isFromPool(p)) {
                freeList_.push(p);
            }
        }
    }

    // 状态查询
    size_t unitSize()   const { return unitSize_; }
    size_t totalUnits() const { return totalUnits_; }
    size_t availableUnits() {
        std::lock_guard<std::mutex> lock(mutex_);
        return freeList_.size();
    }
    void*  basePtr()         { return buffer_.empty() ? nullptr : buffer_.data(); }
    size_t bufferBytes()const { return buffer_.size(); }

private:
    bool isFromPool(void* ptr) const {
        auto* base = const_cast<uint8_t*>(buffer_.data());
        return ptr >= base && ptr < base + buffer_.size();
    }

    std::vector<uint8_t> buffer_;    // 一块连续大内存
    std::queue<void*>    freeList_;  // 空闲单元队列
    size_t unitSize_   = 0;
    size_t totalUnits_ = 0;
    std::mutex mutex_;
    bool initialized_ = false;
};

// ========== 使用示例 ==========
int main() {
    MemPool pool;

    // 初始化：每个单元 4KB，共 1024 个
    constexpr size_t UNIT_SIZE  = 4096;
    constexpr size_t UNIT_COUNT = 1024;
    if (!pool.memInit(UNIT_SIZE, UNIT_COUNT)) {
        std::cerr << "memInit failed\n";
        return -1;
    }
    std::cout << "Pool initialized: " << pool.totalUnits()
              << " units x " << pool.unitSize() << " bytes, base="
              << pool.basePtr() << "\n";

    // 申请 10 个单元
    auto bufs = pool.memAlloc(10);
    std::cout << "Allocated " << bufs.size() << " units, available="
              << pool.availableUnits() << "\n";

    // 使用这些单元
    for (size_t i = 0; i < bufs.size(); ++i) {
        std::memset(bufs[i], static_cast<int>(i), UNIT_SIZE);
    }

    // 归还
    pool.memFree(bufs);
    std::cout << "After free, available=" << pool.availableUnits() << "\n";

    // 也可以单个申请/归还
    void* one = pool.memAllocOne();
    std::cout << "One unit at " << one << "\n";
    pool.memFree(one);

    return 0;
}
