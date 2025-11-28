#pragma once

#include "../base/BufferAllocatorBase.hpp"
#include "../factory/BufferAllocatorFactory.hpp"
#include "../../BufferPool.hpp"
#include <memory>
#include <string>

/**
 * @brief BufferAllocatorFacade - Buffer分配器门面类
 * 
 * 架构角色：门面类（Facade Pattern）
 * 
 * 职责：
 * - 为用户提供统一、简单的Buffer分配接口
 * - 隐藏底层多种Allocator实现的复杂性
 * - 自动选择最优的Allocator实现
 * 
 * 设计模式：门面模式（Facade Pattern）
 * 
 * 使用方式：
 * ```cpp
 * // Worker 中只需要一行创建
 * BufferAllocatorFacade allocator_facade(BufferAllocatorFactory::AllocatorType::AUTO);
 * 
 * // 直接调用，无需关心底层实现
 * auto pool = allocator_facade.allocatePoolWithBuffers(count, size, name, category);
 * ```
 * 
 * 注意：
 * - FramebufferAllocator 通过 Facade 创建时，external_buffers_ 为空
 * - 无参构造函数创建的 FramebufferAllocator 需要后续设置外部内存信息
 * - 如需直接使用外部内存，建议使用带参数的构造函数创建 FramebufferAllocator
 * 
 * 优势：
 * - 用户维护少：只需创建门面对象
 * - 自动创建：构造函数内部自动选择并创建合适的 Allocator
 * - 统一接口：直接调用门面类方法，无需关心底层类型
 * - 无需工厂：不需要显式调用工厂类
 */
class BufferAllocatorFacade {
public:
    /**
     * @brief 构造函数
     * 
     * 设计理念：
     * - Worker层只需要指定Allocator类型
     * - 具体的内存分配器类型、对齐大小等配置细节由Factory内部决定
     * - 符合"高层不依赖底层实现细节"的设计原则
     * 
     * @param type Allocator类型（默认 AUTO）
     * 
     * 配置策略（由Factory内部决定）：
     * - NORMAL: 使用 NORMAL_MALLOC + 64字节对齐
     * - AVFRAME: 使用 AVFrameAllocator 的默认配置
     * - FRAMEBUFFER: 使用 FramebufferAllocator 的默认配置
     * - AUTO: 默认使用 NORMAL 类型
     * 
     * @note 构造函数内部通过 Factory 创建底层 Allocator 实例
     * @note Factory 封装了所有配置细节，上层无需关心
     */
    explicit BufferAllocatorFacade(
        BufferAllocatorFactory::AllocatorType type = BufferAllocatorFactory::AllocatorType::AUTO
    );
    
    /**
     * @brief 析构函数
     */
    ~BufferAllocatorFacade();
    
    // 禁止拷贝
    BufferAllocatorFacade(const BufferAllocatorFacade&) = delete;
    BufferAllocatorFacade& operator=(const BufferAllocatorFacade&) = delete;
    
    // ==================== 统一接口（转发到底层 Allocator）====================
    
    /**
     * @brief 批量创建 Buffer 并构建 BufferPool
     * 
     * @param count Buffer 数量
     * @param size 每个 Buffer 大小
     * @param name BufferPool 名称
     * @param category BufferPool 分类
     * @return unique_ptr<BufferPool> 成功返回 pool，失败返回 nullptr（所有权转移给调用者）
     */
    std::unique_ptr<BufferPool> allocatePoolWithBuffers(
        int count,
        size_t size,
        const std::string& name,
        const std::string& category = ""
    );
    
    /**
     * @brief 创建单个 Buffer 并注入到指定 BufferPool（内部分配）
     * 
     * @param size Buffer 大小
     * @param pool 目标 BufferPool
     * @param queue 注入到哪个队列（FREE 或 FILLED）
     * @return Buffer* 成功返回 buffer，失败返回 nullptr
     */
    Buffer* injectBufferToPool(
        size_t size,
        BufferPool* pool,
        QueueType queue = QueueType::FREE
    );
    
    /**
     * @brief 注入外部已分配的内存到 BufferPool（外部注入）
     * 
     * @param virt_addr 外部内存的虚拟地址（已分配）
     * @param phys_addr 外部内存的物理地址（如果支持，否则为 0）
     * @param size 外部内存的大小（字节）
     * @param pool 目标 BufferPool
     * @param queue 注入到哪个队列（FREE 或 FILLED）
     * @return Buffer* 成功返回 buffer，失败返回 nullptr
     */
    Buffer* injectExternalBufferToPool(
        void* virt_addr,
        uint64_t phys_addr,
        size_t size,
        BufferPool* pool,
        QueueType queue = QueueType::FREE
    );
    
    // ==================== 注意：已删除 getManagedBufferPool() ====================
    // 
    // 设计变更：
    // - Allocator 不再持有 BufferPool（allocatePoolWithBuffers 返回 unique_ptr）
    // - 调用者负责持有和释放 BufferPool
    // - 不再需要 getManagedBufferPool() 方法
    
    /**
     * @brief 从 BufferPool 移除并销毁 Buffer
     * 
     * @param buffer 要移除的 Buffer
     * @param pool 所属的 BufferPool
     * @return true 成功，false 失败
     */
    bool removeBufferFromPool(Buffer* buffer, BufferPool* pool);
    
    /**
     * @brief 销毁整个 BufferPool 及其所有 Buffer
     * 
     * @param pool 要销毁的 BufferPool
     * @return true 成功，false 失败
     */
    bool destroyPool(BufferPool* pool);
    
    /**
     * @brief 获取底层 Allocator 指针（用于特殊操作）
     * 
     * @return BufferAllocatorBase* 底层 Allocator 指针
     * 
     * @note 通常不需要直接访问底层 Allocator
     * @note 特殊场景下可能需要（如 AVFrameAllocator 的 injectAVFrameToPool）
     */
    BufferAllocatorBase* getUnderlyingAllocator() const {
        return allocator_base_uptr_.get();
    }
    
    /**
     * @brief 获取 Allocator 类型
     * 
     * @return AllocatorType 当前使用的 Allocator 类型
     */
    BufferAllocatorFactory::AllocatorType getAllocatorType() const {
        return type_;
    }

private:
    std::unique_ptr<BufferAllocatorBase> allocator_base_uptr_;  // 底层 Allocator 基类指针（通过 Factory 创建）
    BufferAllocatorFactory::AllocatorType type_;           // 当前使用的类型
};

