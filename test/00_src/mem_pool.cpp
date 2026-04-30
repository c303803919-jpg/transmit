// mem_pool.cpp
// MemPool 实现，不含 main，作为库被其他模块调用
#include "mem_pool.h"

#include <new>   // std::bad_alloc

MemPool::MemPool()  = default;
MemPool::~MemPool() = default;

bool MemPool::memInit(size_t unitSize, size_t unitCount) {
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

std::vector<void*> MemPool::memAlloc(size_t count) {
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

void* MemPool::memAllocOne() {
    auto v = memAlloc(1);
    return v.empty() ? nullptr : v.front();
}

void MemPool::memFree(void* ptr) {
    if (!ptr) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!isFromPool(ptr)) return;    // 防止误归还外部指针
    freeList_.push(ptr);
}

void MemPool::memFree(const std::vector<void*>& ptrs) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto* p : ptrs) {
        if (p && isFromPool(p)) {
            freeList_.push(p);
        }
    }
}

size_t MemPool::unitSize()    const { return unitSize_; }
size_t MemPool::totalUnits()  const { return totalUnits_; }
size_t MemPool::bufferBytes() const { return buffer_.size(); }

size_t MemPool::availableUnits() {
    std::lock_guard<std::mutex> lock(mutex_);
    return freeList_.size();
}

void* MemPool::basePtr() {
    return buffer_.empty() ? nullptr : buffer_.data();
}

bool MemPool::isFromPool(void* ptr) const {
    auto* base = const_cast<uint8_t*>(buffer_.data());
    return ptr >= base && ptr < base + buffer_.size();
}
