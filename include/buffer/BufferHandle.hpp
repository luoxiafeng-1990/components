#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

/**
 * @brief 外部 Buffer 的 RAII 包装
 * 
 * 用于管理外部分配的 buffer 的生命周期，提供 weak_ptr 语义：
 * - 自动调用自定义 Deleter 释放资源
 * - 通过 shared_ptr<bool> 实现生命周期检测
 * - 支持各种内存类型（CPU/GPU/DMA...）
 * 
 * 使用场景：
 * - DRM framebuffer
 * - CUDA/OpenGL 共享内存
 * - 自定义硬件 buffer
 */
class BufferHandle {
public:
    /**
     * @brief 自定义释放函数类型
     * @param ptr 虚拟地址
     */
    using Deleter = std::function<void(void*)>;
    
    /**
     * @brief 构造函数
     * @param virt_addr 虚拟地址（CPU访问）
     * @param phys_addr 物理地址（硬件访问，0表示未知）
     * @param size Buffer 大小
     * @param deleter 自定义释放函数（nullptr 表示不自动释放）
     */
    BufferHandle(void* virt_addr, 
                 uint64_t phys_addr,
                 size_t size,
                 Deleter deleter = nullptr);
    
    /**
     * @brief 析构函数 - 自动调用 deleter
     */
    ~BufferHandle();
    
    // 禁止拷贝，允许移动
    BufferHandle(const BufferHandle&) = delete;
    BufferHandle& operator=(const BufferHandle&) = delete;
    BufferHandle(BufferHandle&&) noexcept;
    BufferHandle& operator=(BufferHandle&&) noexcept;
    
    // ========== Getters ==========
    
    /// 获取虚拟地址
    void* getVirtualAddress() const { return virt_addr_; }
    
    /// 获取物理地址
    uint64_t getPhysicalAddress() const { return phys_addr_; }
    
    /// 获取大小
    size_t size() const { return size_; }
    
    /// 检查是否有效（未被移动）
    bool isValid() const { return virt_addr_ != nullptr; }
    
    /**
     * @brief 获取生命周期跟踪器（weak_ptr 语义）
     * 
     * BufferPool 会保存这个 weak_ptr，用于检测外部 buffer 是否已被销毁：
     * 
     * @code
     * auto tracker = handle->getLifetimeTracker();
     * // ... 稍后 ...
     * if (auto alive = tracker.lock()) {
     *     if (*alive) {
     *         // buffer 仍然有效
     *     }
     * } else {
     *     // buffer 已被销毁
     * }
     * @endcode
     * 
     * @return weak_ptr<bool>，指向一个表示"是否存活"的标志
     */
    std::weak_ptr<bool> getLifetimeTracker() const {
        return alive_;
    }
    
private:
    void* virt_addr_;           // 虚拟地址
    uint64_t phys_addr_;        // 物理地址
    size_t size_;               // Buffer 大小
    Deleter deleter_;           // 自定义释放函数
    std::shared_ptr<bool> alive_;  // 生命周期标记（shared_ptr，供 weak_ptr 检测）
};

