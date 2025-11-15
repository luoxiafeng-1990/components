#include "../../include/buffer/DMAHeapAllocator.hpp"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

// ============================================================
// DMAHeapAllocator 实现（空框架）
// ============================================================

DMAHeapAllocator::DMAHeapAllocator()
    : heap_fd_(-1)
{
    printf("🔧 DMAHeapAllocator: Initializing (empty framework)...\n");
    
    // TODO: 打开 DMA-HEAP 设备
    // 例如: heap_fd_ = open("/dev/dma_heap/system", O_RDWR);
    
    printf("⚠️  DMAHeapAllocator: Not yet implemented, will fall back to other allocators\n");
}

DMAHeapAllocator::~DMAHeapAllocator() {
    printf("🔧 DMAHeapAllocator: Cleaning up...\n");
    
    // 释放所有 DMA buffers
    for (const auto& info : dma_buffers_) {
        if (info.virt_addr != nullptr && info.virt_addr != MAP_FAILED) {
            munmap(info.virt_addr, info.size);
        }
        if (info.fd >= 0) {
            close(info.fd);
        }
    }
    dma_buffers_.clear();
    
    // 关闭 heap fd
    if (heap_fd_ >= 0) {
        close(heap_fd_);
        heap_fd_ = -1;
    }
}

void* DMAHeapAllocator::allocate(size_t size, uint64_t* out_phys_addr) {
    printf("⚠️  DMAHeapAllocator::allocate(%zu bytes): Not yet implemented\n", size);
    
    // TODO: 实现 DMA-HEAP 分配逻辑
    // 1. 通过 ioctl 从 DMA-HEAP 分配内存
    // 2. 获取 DMA-BUF fd
    // 3. mmap 映射到用户空间
    // 4. 获取物理地址（如果需要）
    // 5. 保存到 dma_buffers_ 列表
    
    // 当前返回 nullptr，表示分配失败（会触发降级到其他分配器）
    if (out_phys_addr) {
        *out_phys_addr = 0;
    }
    
    return nullptr;
}

void DMAHeapAllocator::deallocate(void* ptr, size_t size) {
    if (ptr == nullptr) {
        return;
    }
    
    printf("⚠️  DMAHeapAllocator::deallocate(%p, %zu): Not yet implemented\n", ptr, size);
    
    // TODO: 实现释放逻辑
    // 1. 在 dma_buffers_ 中查找对应的 buffer
    // 2. munmap 解除映射
    // 3. close fd
    // 4. 从列表中移除
}

int DMAHeapAllocator::getDmaBufFd(void* ptr) const {
    if (ptr == nullptr) {
        return -1;
    }
    
    // 查找对应的 fd
    for (const auto& info : dma_buffers_) {
        if (info.virt_addr == ptr) {
            return info.fd;
        }
    }
    
    return -1;
}

uint64_t DMAHeapAllocator::getPhysicalAddress(void* virt_addr) {
    if (virt_addr == nullptr) {
        return 0;
    }
    
    // 查找已保存的物理地址
    for (const auto& info : dma_buffers_) {
        if (info.virt_addr == virt_addr) {
            return info.phys_addr;
        }
    }
    
    // TODO: 如果没有保存，可以通过 /proc/self/pagemap 获取
    
    return 0;
}

