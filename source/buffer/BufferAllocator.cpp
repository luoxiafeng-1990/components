#include "../../include/buffer/BufferAllocator.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdexcept>
#include <algorithm>

// Linux 特定头文件
#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

// DMA-BUF 相关头文件
#if __has_include(<linux/dma-buf.h>)
#include <linux/dma-buf.h>
#endif

#if __has_include(<linux/dma-heap.h>)
#include <linux/dma-heap.h>
#define HAS_DMA_HEAP 1
#else
#define HAS_DMA_HEAP 0
// 如果系统不支持，定义必要的结构体
struct dma_heap_allocation_data {
    unsigned long len;
    unsigned int fd;
    unsigned int fd_flags;
    unsigned long heap_flags;
};
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_allocation_data)
#endif

#endif  // __linux__

// ============================================================
// NormalAllocator 实现
// ============================================================

void* NormalAllocator::allocate(size_t size, uint64_t* out_phys_addr) {
    // 使用 posix_memalign 分配对齐的内存（4KB 对齐）
    void* addr = nullptr;
    int ret = posix_memalign(&addr, 4096, size);
    if (ret != 0) {
        printf("❌ posix_memalign failed: %s\n", strerror(ret));
        return nullptr;
    }
    
    // 清零（可选，根据需求）
    memset(addr, 0, size);
    
    // 尝试获取物理地址
    if (out_phys_addr) {
        *out_phys_addr = getPhysicalAddress(addr);
        if (*out_phys_addr == 0) {
            printf("⚠️  Warning: Failed to get physical address for normal memory\n");
        }
    }
    
    return addr;
}

void NormalAllocator::deallocate(void* ptr, size_t size) {
    (void)size;  // 普通内存不需要 size
    if (ptr) {
        free(ptr);
    }
}

uint64_t NormalAllocator::getPhysicalAddress(void* virt_addr) {
#ifdef __linux__
    // 通过 /proc/self/pagemap 获取物理地址
    int fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) {
        // 权限不足或系统不支持
        return 0;
    }
    
    uintptr_t virt = reinterpret_cast<uintptr_t>(virt_addr);
    size_t page_size = sysconf(_SC_PAGE_SIZE);
    uint64_t page_offset = virt % page_size;
    uint64_t pfn_item_offset = (virt / page_size) * sizeof(uint64_t);
    
    uint64_t pfn_item;
    if (lseek(fd, pfn_item_offset, SEEK_SET) < 0) {
        close(fd);
        return 0;
    }
    
    if (read(fd, &pfn_item, sizeof(uint64_t)) != sizeof(uint64_t)) {
        close(fd);
        return 0;
    }
    
    close(fd);
    
    // 检查页是否存在于物理内存
    if ((pfn_item & (1ULL << 63)) == 0) {
        // 页未分配或已换出
        return 0;
    }
    
    // 提取物理页帧号 (PFN)
    uint64_t pfn = pfn_item & ((1ULL << 55) - 1);
    uint64_t phys_addr = (pfn * page_size) + page_offset;
    
    return phys_addr;
#else
    // 非 Linux 系统不支持
    (void)virt_addr;
    return 0;
#endif
}

// ============================================================
// CMAAllocator 实现
// ============================================================

CMAAllocator::CMAAllocator() {
    // 构造时可以检测系统是否支持 DMA-BUF
#ifdef __linux__
    printf("🔧 Initializing CMAAllocator...\n");
#if HAS_DMA_HEAP
    printf("   DMA-BUF heap support: ✅ Available\n");
#else
    printf("   DMA-BUF heap support: ⚠️  Headers not found (will try runtime detection)\n");
#endif
#else
    printf("⚠️  Warning: CMAAllocator not supported on this platform\n");
#endif
}

CMAAllocator::~CMAAllocator() {
    // 清理所有 DMA buffer
    for (auto& info : dma_buffers_) {
        if (info.virt_addr) {
            munmap(info.virt_addr, info.size);
        }
        if (info.fd >= 0) {
            close(info.fd);
        }
    }
    dma_buffers_.clear();
}

void* CMAAllocator::allocate(size_t size, uint64_t* out_phys_addr) {
#ifdef __linux__
    int dma_fd = -1;
    uint64_t phys_addr = 0;
    
    void* virt_addr = allocateDmaBuf(size, &dma_fd, &phys_addr);
    
    if (virt_addr) {
        // 保存映射信息
        dma_buffers_.push_back({virt_addr, dma_fd, size});
        
        if (out_phys_addr) {
            *out_phys_addr = phys_addr;
        }
        
        printf("✅ CMA buffer allocated: virt=%p, phys=0x%lx, size=%zu, fd=%d\n",
               virt_addr, phys_addr, size, dma_fd);
    }
    
    return virt_addr;
#else
    // 非 Linux 系统不支持
    (void)size;
    (void)out_phys_addr;
    printf("❌ ERROR: CMA allocation not supported on this platform\n");
    return nullptr;
#endif
}

void CMAAllocator::deallocate(void* ptr, size_t size) {
    if (!ptr) return;
    
    // 查找对应的 DMA buffer 信息
    auto it = std::find_if(dma_buffers_.begin(), dma_buffers_.end(),
                          [ptr](const DmaBufferInfo& info) {
                              return info.virt_addr == ptr;
                          });
    
    if (it != dma_buffers_.end()) {
        // 找到了，释放
        munmap(it->virt_addr, it->size);
        if (it->fd >= 0) {
            close(it->fd);
        }
        dma_buffers_.erase(it);
        printf("🧹 CMA buffer deallocated: %p\n", ptr);
    } else {
        // 没找到，但仍然尝试释放
        munmap(ptr, size);
        printf("⚠️  Warning: CMA buffer %p not found in registry, forced unmap\n", ptr);
    }
}

int CMAAllocator::getDmaBufFd(void* ptr) const {
    auto it = std::find_if(dma_buffers_.begin(), dma_buffers_.end(),
                          [ptr](const DmaBufferInfo& info) {
                              return info.virt_addr == ptr;
                          });
    
    if (it != dma_buffers_.end()) {
        return it->fd;
    }
    
    return -1;
}

void* CMAAllocator::allocateDmaBuf(size_t size, int* out_fd, uint64_t* out_phys_addr) {
#ifdef __linux__
    // 尝试打开 DMA heap 设备
    const char* heap_paths[] = {
        "/dev/dma_heap/linux,cma",   // CMA heap
        "/dev/dma_heap/system",      // System heap
        "/dev/ion",                  // 旧版 ION（Android）
    };
    
    int heap_fd = -1;
    const char* used_path = nullptr;
    
    for (const char* path : heap_paths) {
        heap_fd = open(path, O_RDWR);
        if (heap_fd >= 0) {
            used_path = path;
            break;
        }
    }
    
    if (heap_fd < 0) {
        printf("❌ Failed to open DMA heap device (tried %zu paths)\n", 
               sizeof(heap_paths) / sizeof(heap_paths[0]));
        return nullptr;
    }
    
    printf("   📂 Opened DMA heap: %s\n", used_path);
    
    // 分配 DMA buffer
    struct dma_heap_allocation_data heap_data;
    memset(&heap_data, 0, sizeof(heap_data));
    heap_data.len = size;
    heap_data.fd_flags = O_RDWR | O_CLOEXEC;
    heap_data.heap_flags = 0;
    
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &heap_data) < 0) {
        printf("❌ DMA_HEAP_IOCTL_ALLOC failed: %s\n", strerror(errno));
        close(heap_fd);
        return nullptr;
    }
    
    *out_fd = heap_data.fd;
    close(heap_fd);  // heap_fd 可以关闭，DMA buffer fd 保持打开
    
    // mmap DMA buffer 到用户空间
    void* virt_addr = mmap(NULL, size, PROT_READ | PROT_WRITE, 
                           MAP_SHARED, *out_fd, 0);
    if (virt_addr == MAP_FAILED) {
        printf("❌ mmap DMA buffer failed: %s\n", strerror(errno));
        close(*out_fd);
        *out_fd = -1;
        return nullptr;
    }
    
    // 获取物理地址
    if (out_phys_addr) {
        *out_phys_addr = getPhysicalAddress(virt_addr);
        if (*out_phys_addr == 0) {
            printf("⚠️  Warning: Failed to get physical address for CMA buffer\n");
        }
    }
    
    return virt_addr;
#else
    // 非 Linux 系统
    (void)size;
    (void)out_fd;
    (void)out_phys_addr;
    return nullptr;
#endif
}

uint64_t CMAAllocator::getPhysicalAddress(void* virt_addr) {
    // 复用 NormalAllocator 的实现
    NormalAllocator normal;
    return normal.getPhysicalAddress(virt_addr);
}

// ============================================================
// ExternalAllocator 实现
// ============================================================

void* ExternalAllocator::allocate(size_t size, uint64_t* out_phys_addr) {
    (void)size;
    (void)out_phys_addr;
    throw std::logic_error("ExternalAllocator::allocate() should not be called. "
                          "External buffers must be provided by user.");
}

void ExternalAllocator::deallocate(void* ptr, size_t size) {
    // 不释放外部内存（由用户管理）
    (void)ptr;
    (void)size;
    // printf("ℹ️  ExternalAllocator::deallocate() called (no-op for external buffer %p)\n", ptr);
}

