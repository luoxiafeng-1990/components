#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * @brief 抽象内存分配器接口（策略模式）
 * 
 * 定义内存分配和释放的统一接口，支持：
 * - 普通内存（posix_memalign）
 * - CMA/DMA 连续物理内存（DMA-BUF heap）
 * - 外部内存（不实际分配）
 */
class BufferAllocator {
public:
    virtual ~BufferAllocator() = default;
    
    /**
     * @brief 分配内存
     * @param size 需要分配的大小（字节）
     * @param out_phys_addr 输出物理地址（如果获取成功），可为 nullptr
     * @return 虚拟地址，失败返回 nullptr
     */
    virtual void* allocate(size_t size, uint64_t* out_phys_addr) = 0;
    
    /**
     * @brief 释放内存
     * @param ptr 虚拟地址
     * @param size 内存大小（某些分配器需要）
     */
    virtual void deallocate(void* ptr, size_t size) = 0;
    
    /**
     * @brief 获取分配器类型名称（用于调试）
     */
    virtual const char* name() const = 0;
};

// ============================================================
// 普通内存分配器（使用 posix_memalign）
// ============================================================

class NormalAllocator : public BufferAllocator {
public:
    NormalAllocator() = default;
    ~NormalAllocator() override = default;
    
    void* allocate(size_t size, uint64_t* out_phys_addr) override;
    void deallocate(void* ptr, size_t size) override;
    const char* name() const override { return "NormalAllocator"; }
    
    /**
     * @brief 通过 /proc/self/pagemap 获取物理地址
     * @param virt_addr 虚拟地址
     * @return 物理地址，失败返回 0
     */
    uint64_t getPhysicalAddress(void* virt_addr);
};

// ============================================================
// CMA/DMA 连续物理内存分配器（使用 DMA-BUF heap）
// ============================================================

class CMAAllocator : public BufferAllocator {
public:
    CMAAllocator();
    ~CMAAllocator() override;
    
    void* allocate(size_t size, uint64_t* out_phys_addr) override;
    void deallocate(void* ptr, size_t size) override;
    const char* name() const override { return "CMAAllocator"; }
    
    /**
     * @brief 获取指定buffer的 DMA-BUF fd
     * @param ptr 虚拟地址
     * @return DMA-BUF fd，失败返回 -1
     */
    int getDmaBufFd(void* ptr) const;
    
    /**
     * @brief 获取物理地址（通过 /proc/self/pagemap）
     */
    uint64_t getPhysicalAddress(void* virt_addr);
    
private:
    /**
     * @brief 通过 DMA-BUF heap 分配连续物理内存
     * @param size 大小
     * @param out_fd 输出 DMA-BUF fd
     * @param out_phys_addr 输出物理地址
     * @return 虚拟地址，失败返回 nullptr
     */
    void* allocateDmaBuf(size_t size, int* out_fd, uint64_t* out_phys_addr);
    
    // 存储 (虚拟地址, DMA fd, 大小) 的映射
    struct DmaBufferInfo {
        void* virt_addr;
        int fd;
        size_t size;
    };
    std::vector<DmaBufferInfo> dma_buffers_;
};

// ============================================================
// 外部内存"分配器"（不实际分配，只是接口统一）
// ============================================================

class ExternalAllocator : public BufferAllocator {
public:
    ExternalAllocator() = default;
    ~ExternalAllocator() override = default;
    
    /**
     * @brief 不应该被调用（外部buffer由用户提供）
     * @throws std::logic_error
     */
    void* allocate(size_t size, uint64_t* out_phys_addr) override;
    
    /**
     * @brief 不释放外部内存（由用户管理）
     */
    void deallocate(void* ptr, size_t size) override;
    
    const char* name() const override { return "ExternalAllocator"; }
};

