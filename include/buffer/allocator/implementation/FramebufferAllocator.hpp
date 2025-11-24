#pragma once

#include "../base/BufferAllocatorBase.hpp"
#include <vector>

// 前向声明（避免循环依赖）
class LinuxFramebufferDevice;

/**
 * @brief FramebufferAllocator - Framebuffer 外部内存分配器
 * 
 * 继承自 BufferAllocatorBase（抽象基类）
 * 
 * 管理由外部设备（如 framebuffer）提供的已映射内存
 * 
 * 特点：
 * - 虚拟内存：由调用者提供（已 mmap）
 * - 物理地址：由调用者提供（可选）
 * - 连续性：通常是物理连续的
 * 
 * 职责：
 * - 不分配新内存
 * - 仅包装外部内存为 Buffer 对象
 * - 不释放内存（仅删除 Buffer 对象）
 * 
 * 使用场景：
 * - Framebuffer 设备内存
 * - DRM/KMS 显示内存
 * - GPU 共享内存
 * 
 * 使用示例：
 * @code
 * // 1. mmap framebuffer 内存
 * void* fb_base = mmap(..., fb_fd, 0);
 * 
 * // 2. 计算每个 buffer 的地址
 * std::vector<FramebufferAllocator::BufferInfo> infos;
 * for (int i = 0; i < count; i++) {
 *     infos.push_back({
 *         .virt_addr = (void*)((char*)fb_base + i * size),
 *         .phys_addr = phys_base + i * size,
 *         .size = size
 *     });
 * }
 * 
 * // 3. 创建 Allocator 并注入到 pool
 * auto allocator = std::make_unique<FramebufferAllocator>(infos);
 * auto pool = allocator->allocatePoolWithBuffers(count, size, "FBPool", "Display");
 * @endcode
 */
class FramebufferAllocator : public BufferAllocatorBase {
public:
    /**
     * @brief 外部 Buffer 信息结构
     */
    struct BufferInfo {
        void* virt_addr;       // 虚拟地址（已 mmap）
        uint64_t phys_addr;    // 物理地址（可选，0 表示无）
        size_t size;           // Buffer 大小
    };
    
    /**
     * @brief 默认构造函数（无参）
     * 
     * 创建一个空的 FramebufferAllocator 实例。
     * 
     * 注意：
     * - 创建的实例 external_buffers_ 为空
     * - 调用 allocatePoolWithBuffers() 时会失败（因为没有外部内存信息）
     * - 需要通过其他方式设置外部内存信息（如通过 setExternalBuffers() 方法，如果存在）
     * 
     * 使用场景：
     * - 通过 Factory 创建时使用
     * - 需要延迟设置外部内存信息的场景
     * 
     * @note 通常建议使用带参数的构造函数直接初始化
     */
    FramebufferAllocator();
    
    /**
     * @brief 构造函数 1：从预构建的 BufferInfo 列表构造
     * 
     * @param external_buffers 外部提供的 buffer 信息列表
     * 
     * 适用场景：
     * - 高级用户需要完全控制 buffer 信息
     * - 非 LinuxFramebufferDevice 的外部内存源
     */
    explicit FramebufferAllocator(const std::vector<BufferInfo>& external_buffers);
    
    /**
     * @brief 构造函数 2：从 LinuxFramebufferDevice 构造（推荐）
     * 
     * 工作流程：
     * 1. 调用 device->getMappedInfo() 获取 mmap 信息
     * 2. 调用私有方法 buildBufferInfosFromDevice() 构建 BufferInfo 列表
     * 3. 存储到 external_buffers_
     * 
     * @param device LinuxFramebufferDevice 指针（必须已初始化）
     * 
     * 使用示例：
     * @code
     * auto device = std::make_unique<LinuxFramebufferDevice>();
     * device->initialize(0);
     * 
     * auto allocator = std::make_unique<FramebufferAllocator>(device.get());
     * auto pool = allocator->allocatePoolWithBuffers(0, 0, "FBPool", "Display");
     * device->setBufferPool(pool.get());
     * @endcode
     */
    explicit FramebufferAllocator(LinuxFramebufferDevice* device);
    
    ~FramebufferAllocator() override;
    
    // ==================== 实现基类纯虚函数 ====================
    
    /**
     * @brief 批量创建 Buffer 并构建 BufferPool（实际上是批量包装外部内存）
     * 
     * 注意：
     * - count 和 size 参数会被忽略
     * - 使用构造时传入的 external_buffers_ 数量和大小
     * 
     * @param count 忽略（使用 external_buffers_.size()）
     * @param size 忽略（使用 external_buffers_[i].size）
     * @param name BufferPool 名称
     * @param category BufferPool 分类
     * @return unique_ptr<BufferPool> 成功返回 pool，失败返回 nullptr
     */
    std::shared_ptr<BufferPool> allocatePoolWithBuffers(
        int count,
        size_t size,
        const std::string& name,
        const std::string& category = ""
    ) override;
    
    /**
     * @brief 创建单个 Buffer 并注入到指定 BufferPool（内部分配）
     * 
     * @note FramebufferAllocator 不支持内部分配，应使用 allocatePoolWithBuffers 或 injectExternalBufferToPool
     */
    Buffer* injectBufferToPool(
        size_t size,
        BufferPool* pool,
        QueueType queue = QueueType::FREE
    ) override;
    
    /**
     * @brief 注入外部已分配的内存到 BufferPool（外部注入）
     * 
     * @note FramebufferAllocator 支持此方法，可以包装外部内存为 Buffer
     * @note 这是 LinuxFramebufferDevice 使用的主要方法（逐个注入）
     */
    Buffer* injectExternalBufferToPool(
        void* virt_addr,
        uint64_t phys_addr,
        size_t size,
        BufferPool* pool,
        QueueType queue = QueueType::FREE
    ) override;
    
    /**
     * @brief 从 BufferPool 移除并销毁 Buffer
     */
    bool removeBufferFromPool(Buffer* buffer, BufferPool* pool) override;
    
    /**
     * @brief 销毁整个 BufferPool 及其所有 Buffer
     */
    bool destroyPool(BufferPool* pool) override;
    
protected:
    /**
     * @brief 创建单个 Buffer（包装外部内存）
     * 
     * 实现逻辑：
     * 1. 从 external_buffers_[id] 获取地址和大小
     * 2. 创建 Buffer 对象（Ownership::EXTERNAL）
     * 3. 返回 Buffer 指针
     * 
     * @param id Buffer ID（也是 external_buffers_ 索引）
     * @param size Buffer 大小（忽略，使用 external_buffers_[id].size）
     * @return Buffer* 成功返回 buffer，失败返回 nullptr
     */
    Buffer* createBuffer(uint32_t id, size_t size) override;
    
    /**
     * @brief 销毁 Buffer（仅删除对象，不释放内存）
     * 
     * 实现逻辑：
     * 1. 不释放内存（外部管理）
     * 2. 仅删除 Buffer 对象
     * 
     * @param buffer 要销毁的 Buffer
     */
    void deallocateBuffer(Buffer* buffer) override;
    
private:
    /**
     * @brief 清理 Pool 中所有属于此 Allocator 的 buffer（辅助方法）
     */
    void cleanupPool(BufferPool* pool);
    
    /**
     * @brief 从 LinuxFramebufferDevice 构建 BufferInfo 列表（私有辅助方法）
     * 
     * 工作流程：
     * 1. 调用 device->getMappedInfo() 获取信息
     * 2. 计算每个 buffer 的虚拟地址
     * 3. 构建并返回 BufferInfo 列表
     * 
     * @param device LinuxFramebufferDevice 指针
     * @return vector<BufferInfo> BufferInfo 列表
     */
    static std::vector<BufferInfo> buildBufferInfosFromDevice(
        LinuxFramebufferDevice* device
    );
    
    std::vector<BufferInfo> external_buffers_;  // 外部内存信息
    size_t next_buffer_index_;                   // 下一个可用索引
};

