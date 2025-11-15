#pragma once

#include "BufferAllocator.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * @brief DMA-HEAP 内存分配器
 * 
 * 通过 DMA-HEAP 机制分配连续物理内存，适用于 DMA 传输场景。
 * 
 * 功能特性：
 * - 分配连续物理内存
 * - 支持获取物理地址
 * - 支持导出 DMA-BUF fd 用于跨进程共享
 * 
 * @note 当前为空框架实现，需要根据具体硬件平台和内核版本实现
 */
class DMAHeapAllocator : public BufferAllocator {
public:
    DMAHeapAllocator();
    ~DMAHeapAllocator() override;
    
    /**
     * @brief 分配 DMA-HEAP 内存
     * @param size 需要分配的大小（字节）
     * @param out_phys_addr 输出物理地址（如果获取成功），可为 nullptr
     * @return 虚拟地址，失败返回 nullptr
     */
    void* allocate(size_t size, uint64_t* out_phys_addr) override;
    
    /**
     * @brief 释放 DMA-HEAP 内存
     * @param ptr 虚拟地址
     * @param size 内存大小
     */
    void deallocate(void* ptr, size_t size) override;
    
    /**
     * @brief 获取分配器名称
     */
    const char* name() const override { return "DMAHeapAllocator"; }
    
    /**
     * @brief 获取指定 buffer 的 DMA-BUF fd
     * @param ptr 虚拟地址
     * @return DMA-BUF fd，失败返回 -1
     */
    int getDmaBufFd(void* ptr) const;
    
    /**
     * @brief 获取物理地址
     * @param virt_addr 虚拟地址
     * @return 物理地址，失败返回 0
     */
    uint64_t getPhysicalAddress(void* virt_addr);
    
private:
    // 存储 DMA buffer 信息的结构
    struct DmaBufferInfo {
        void* virt_addr;    // 虚拟地址
        int fd;             // DMA-BUF fd
        size_t size;        // 大小
        uint64_t phys_addr; // 物理地址
    };
    
    // DMA buffer 列表
    std::vector<DmaBufferInfo> dma_buffers_;
    
    // DMA-HEAP 设备 fd（如果需要）
    int heap_fd_;
};

