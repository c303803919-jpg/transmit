// mem_pool.h
// 固定大小数据单元的内存池，适用于传输缓存场景
#ifndef MEM_POOL_H
#define MEM_POOL_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <queue>
#include <vector>

class MemPool {
public:
    MemPool();
    ~MemPool();

    // 禁用拷贝，避免 buffer 被意外复制
    MemPool(const MemPool&)            = delete;
    MemPool& operator=(const MemPool&) = delete;

    // 初始化：预分配 unitCount 个 unitSize 字节的单元
    // 如需注册给网卡/GPU，可在此处 resize 之后调用 ibv_reg_mr / cudaHostRegister
    bool memInit(size_t unitSize, size_t unitCount);

    // 申请 count 个内存单元，返回指针数组（不足则返回空）
    std::vector<void*> memAlloc(size_t count);

    // 申请单个单元的便捷接口
    void* memAllocOne();

    // 归还一个单元 / 批量归还
    void memFree(void* ptr);
    void memFree(const std::vector<void*>& ptrs);

    // 状态查询
    size_t unitSize()   const;
    size_t totalUnits() const;
    size_t availableUnits();
    void*  basePtr();
    size_t bufferBytes() const;

private:
    bool isFromPool(void* ptr) const;

    std::vector<uint8_t> buffer_;    // 一块连续大内存
    std::queue<void*>    freeList_;  // 空闲单元队列
    size_t     unitSize_   = 0;
    size_t     totalUnits_ = 0;
    std::mutex mutex_;
    bool       initialized_ = false;
};

#endif // MEM_POOL_H
