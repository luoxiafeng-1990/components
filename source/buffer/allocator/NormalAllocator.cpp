#include "buffer/allocator/NormalAllocator.hpp"
#include <cstdlib>
#include <cstring>
#include <stdio.h>

// ============================================================
// 构造/析构函数
// ============================================================

NormalAllocator::NormalAllocator(BufferMemoryAllocatorType type, size_t alignment)
    : type_(type)
    , alignment_(alignment)
{
    printf("🔧 NormalAllocator created (alignment=%zu)\n", alignment_);
}

NormalAllocator::~NormalAllocator() {
    printf("🧹 NormalAllocator destroyed\n");
}

// ============================================================
// 核心实现
// ============================================================

Buffer* NormalAllocator::createBuffer(uint32_t id, size_t size) {
    // 1. 分配对齐内存
    void* virt_addr = nullptr;
    
    if (alignment_ > 0) {
        // 使用对齐分配
        if (posix_memalign(&virt_addr, alignment_, size) != 0) {
            printf("❌ posix_memalign failed for buffer #%u (size=%zu)\n", id, size);
            return nullptr;
        }
    } else {
        // 普通分配
        virt_addr = malloc(size);
        if (!virt_addr) {
            printf("❌ malloc failed for buffer #%u (size=%zu)\n", id, size);
            return nullptr;
        }
    }
    
    // 2. 清零内存（可选，用于调试）
    memset(virt_addr, 0, size);
    
    // 3. 创建 Buffer 对象
    // 普通内存没有物理地址，phys_addr = 0
    Buffer* buffer = new Buffer(
        id,
        virt_addr,
        0,  // phys_addr = 0（普通内存不提供物理地址）
        size,
        Buffer::Ownership::OWNED
    );
    
    if (!buffer) {
        printf("❌ Failed to create Buffer object #%u\n", id);
        free(virt_addr);
        return nullptr;
    }
    
    return buffer;
}

void NormalAllocator::deallocateBuffer(Buffer* buffer) {
    if (!buffer) {
        return;
    }
    
    // 1. 释放内存
    void* virt_addr = buffer->getVirtualAddress();
    if (virt_addr) {
        free(virt_addr);
    }
    
    // 2. 删除 Buffer 对象
    delete buffer;
}

