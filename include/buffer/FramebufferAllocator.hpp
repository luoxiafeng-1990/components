#pragma once

#include "BufferAllocator.hpp"
#include <vector>

/**
 * @brief FramebufferAllocator - Framebuffer 外部内存分配器
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
class FramebufferAllocator : public BufferAllocator {
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
     * @brief 构造函数
     * 
     * @param external_buffers 外部提供的 buffer 信息列表
     */
    explicit FramebufferAllocator(const std::vector<BufferInfo>& external_buffers);
    
    ~FramebufferAllocator() override;
    
    /**
     * @brief 重写：批量分配（实际上是批量包装）
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
    std::unique_ptr<BufferPool> allocatePoolWithBuffers(
        int count,
        size_t size,
        const std::string& name,
        const std::string& category = ""
    );
    
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
    std::vector<BufferInfo> external_buffers_;  // 外部内存信息
    size_t next_buffer_index_;                   // 下一个可用索引
};

