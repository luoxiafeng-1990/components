#include "buffer/allocator/base/BufferAllocatorBase.hpp"
#include "buffer/BufferPoolRegistry.hpp"
#include <stdio.h>

// ========== BufferAllocatorBase 实现 ==========

BufferAllocatorBase::~BufferAllocatorBase() {
    // v2.0 析构函数职责：
    // 1. 通过友元从 Registry 获取 BufferPool（临时访问）
    // 2. 销毁所有 Buffer 对象和内存
    // 3. 从 Registry 注销（触发 Pool 析构）
    
    if (pool_id_ != 0) {
        printf("🧹 [BufferAllocatorBase] Cleaning up BufferPool (ID: %lu)...\n", pool_id_);
        
        // 调用子类的 destroyPool() 实现
        if (!destroyPool(pool_id_)) {
            printf("⚠️  [BufferAllocatorBase] Failed to destroy BufferPool (ID: %lu)\n", pool_id_);
        }
    }
}

