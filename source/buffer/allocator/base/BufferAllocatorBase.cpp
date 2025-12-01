#include "buffer/allocator/base/BufferAllocatorBase.hpp"
#include "buffer/BufferPoolRegistry.hpp"
#include <stdio.h>

// ========== BufferAllocatorBase 实现 ==========

BufferAllocatorBase::~BufferAllocatorBase() {
    // v2.0 析构函数设计：
    // - 不在基类析构中调用虚函数 destroyPool()
    // - 原因：在基类析构中，对象已经是基类类型，虚函数机制失效
    // - 解决方案：让子类在自己的析构函数中调用 destroyPool()
    // - 子类析构先执行，此时对象还是子类类型，可以正确调用子类的 destroyPool()
    
    // 注意：如果子类忘记在析构中清理，这里可以添加警告
    if (pool_id_ != 0) {
        printf("⚠️  [BufferAllocatorBase] Warning: pool_id_=%lu not cleaned up by subclass destructor\n", pool_id_);
    }
}

// ========== 受保护方法实现 ==========

std::shared_ptr<BufferPool> BufferAllocatorBase::getPoolForCleanup(uint64_t pool_id) {
    // v2.0: 通过友元访问 Registry 的私有清理方法
    // BufferAllocatorBase 是 BufferPoolRegistry 的友元，可以访问私有方法
    return BufferPoolRegistry::getInstance().getPoolForAllocatorCleanup(pool_id);
}

