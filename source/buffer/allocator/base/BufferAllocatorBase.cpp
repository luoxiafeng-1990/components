#include "buffer/allocator/base/BufferAllocatorBase.hpp"
#include "buffer/BufferPoolRegistry.hpp"
#include <stdio.h>

// ========== 静态成员初始化 ==========

std::atomic<uint64_t> BufferAllocatorBase::next_allocator_id_(1);  // 从1开始

// ========== BufferAllocatorBase 实现 ==========

BufferAllocatorBase::~BufferAllocatorBase() {
    // v2.0 析构函数设计：
    // - 基类析构函数不调用 destroyPool()（因为 destroyPool() 是纯虚函数）
    // - 子类必须在自己的析构函数中调用 getAllPoolIds()，然后逐个调用 destroyPool()
    // - 这样确保每个 Allocator 自己控制如何清理 Buffer
}

// ========== 受保护方法实现 ==========

std::shared_ptr<BufferPool> BufferAllocatorBase::getPoolForCleanup(uint64_t pool_id) {
    // v2.0: 通过友元访问 Registry 的私有清理方法
    // BufferAllocatorBase 是 BufferPoolRegistry 的友元，可以访问私有方法
    return BufferPoolRegistry::getInstance().getPoolForAllocatorCleanup(pool_id);
}

void BufferAllocatorBase::unregisterPoolForCleanup(uint64_t pool_id) {
    // v2.0: 通过友元访问 Registry 的私有注销方法
    // BufferAllocatorBase 是 BufferPoolRegistry 的友元，可以访问私有方法
    // 子类在 destroyPool() 中清理完 Buffer 后调用此方法注销 Pool
    BufferPoolRegistry::getInstance().unregisterPool(pool_id);
}

std::vector<uint64_t> BufferAllocatorBase::getAllPoolIds() const {
    // v2.0: 查询 Registry 获取所有属于此 Allocator 的 Pool ID
    // 子类在析构函数中使用此方法获取所有 Pool，然后逐个调用 destroyPool()
    auto& registry = BufferPoolRegistry::getInstance();
    return registry.getPoolsByAllocatorId(getAllocatorId());
}

