#pragma once

#include "BufferAllocator.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * @brief TACO 系统专用内存分配器
 * 
 * 使用 TACO 系统提供的专用内存分配接口，适用于 TACO 平台的特定硬件加速场景。
 * 
 * 功能特性：
 * - 通过 TACO 系统 API 分配内存
 * - 支持获取物理地址
 * - 与 TACO 硬件加速器深度集成
 * 
 * @note 当前为空框架实现，需要根据 TACO 系统 API 实现
 */
class TacoSysAllocator : public BufferAllocator {
public:
    TacoSysAllocator();
    ~TacoSysAllocator() override;
    
    /**
     * @brief 通过 TACO 系统分配内存
     * @param size 需要分配的大小（字节）
     * @param out_phys_addr 输出物理地址（如果获取成功），可为 nullptr
     * @return 虚拟地址，失败返回 nullptr
     */
    void* allocate(size_t size, uint64_t* out_phys_addr) override;
    
    /**
     * @brief 释放 TACO 系统内存
     * @param ptr 虚拟地址
     * @param size 内存大小
     */
    void deallocate(void* ptr, size_t size) override;
    
    /**
     * @brief 获取分配器名称
     */
    const char* name() const override { return "TacoSysAllocator"; }
    
    /**
     * @brief 获取物理地址
     * @param virt_addr 虚拟地址
     * @return 物理地址，失败返回 0
     */
    uint64_t getPhysicalAddress(void* virt_addr);
    
    /**
     * @brief 获取 TACO 系统 buffer 的句柄（如果需要）
     * @param ptr 虚拟地址
     * @return TACO buffer 句柄，失败返回 -1
     */
    int getTacoBufferHandle(void* ptr) const;
    
private:
    // 存储 TACO buffer 信息的结构
    struct TacoBufferInfo {
        void* virt_addr;    // 虚拟地址
        int handle;         // TACO buffer 句柄
        size_t size;        // 大小
        uint64_t phys_addr; // 物理地址
    };
    
    // TACO buffer 列表
    std::vector<TacoBufferInfo> taco_buffers_;
    
    // TACO 系统上下文（如果需要）
    void* taco_ctx_;
};

