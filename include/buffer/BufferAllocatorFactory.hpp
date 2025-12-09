#ifndef BUFFER_ALLOCATOR_FACTORY_HPP
#define BUFFER_ALLOCATOR_FACTORY_HPP

#include "buffer/BufferAllocatorBase.hpp"
#include <memory>

/**
 * @brief BufferAllocatorFactory - Buffer分配器工厂
 * 
 * 架构角色：Allocator Factory（分配器工厂）
 * 
 * 设计模式：工厂模式（Factory Pattern）
 * 
 * 职责：
 * - 根据类型创建合适的Allocator实现
 * - 封装Allocator创建逻辑
 * - 支持自动检测和手动指定两种模式
 * 
 * 创建的Allocator类型：
 * - NormalAllocator: 普通内存分配器（用于Raw视频文件）
 * - AVFrameAllocator: AVFrame包装分配器（用于FFmpeg解码，动态注入）
 * - FramebufferAllocator: Framebuffer内存包装分配器（用于Framebuffer设备）
 * 
 * 注意：
 * - FramebufferAllocator 通过 Factory 创建时，external_buffers_ 为空
 * - 无参构造函数创建的 FramebufferAllocator 需要后续设置外部内存信息
 * - 如需直接使用外部内存，建议使用带参数的构造函数：
 *   ```cpp
 *   // 方式1：从 LinuxFramebufferDevice 创建
 *   auto allocator = std::make_unique<FramebufferAllocator>(device.get());
 *   
 *   // 方式2：从 BufferInfo 列表创建
 *   std::vector<FramebufferAllocator::BufferInfo> infos = {...};
 *   auto allocator = std::make_unique<FramebufferAllocator>(infos);
 *   ```
 * 
 * 优势：
 * - 用户无需了解具体实现类
 * - 可以根据场景自动选择最优实现
 * - 统一的创建接口，便于维护
 */
class BufferAllocatorFactory {
public:
    /**
     * Allocator类型枚举
     */
    enum class AllocatorType {
        AUTO,           // 自动选择（默认使用 NormalAllocator）
        NORMAL,         // NormalAllocator（普通内存分配）
        AVFRAME,        // AVFrameAllocator（FFmpeg AVFrame包装）
        FRAMEBUFFER     // FramebufferAllocator（Framebuffer内存包装）
    };
    
    /**
     * @brief 创建Buffer分配器（简化版工厂方法） - 推荐使用
     * 
     * 设计理念：
     * - 上层只需指定Allocator类型
     * - 配置细节（mem_type, alignment）由工厂内部决定
     * - 每种类型使用最优的默认配置
     * 
     * 配置策略（工厂内部决定）：
     * - NORMAL: NORMAL_MALLOC + 64字节对齐
     * - AVFRAME: AVFrameAllocator默认配置
     * - FRAMEBUFFER: FramebufferAllocator默认配置
     * - AUTO: 默认使用NORMAL
     * 
     * @param type Allocator类型（默认AUTO）
     * @return Allocator实例（智能指针）
     */
    static std::unique_ptr<BufferAllocatorBase> create(
        AllocatorType type = AllocatorType::AUTO
    );
    
    /**
     * @brief 创建Buffer分配器（完整版，用于特殊配置需求）
     * 
     * @deprecated 推荐使用简化版 create(AllocatorType)
     * 
     * @param type Allocator类型
     * @param mem_type 内存分配器类型（仅用于NormalAllocator）
     * @param alignment 内存对齐（仅用于NormalAllocator）
     * @return Allocator实例（智能指针）
     */
    static std::unique_ptr<BufferAllocatorBase> createWithConfig(
        AllocatorType type,
        BufferMemoryAllocatorType mem_type,
        size_t alignment
    );
    
    /**
     * @brief 从名称创建Allocator
     * 
     * @param name 类型名称（"normal", "avframe", "framebuffer", "auto"）
     * @param mem_type 内存分配器类型（用于NormalAllocator）
     * @param alignment 内存对齐（用于NormalAllocator）
     * @return Allocator实例
     */
    static std::unique_ptr<BufferAllocatorBase> createByName(
        const char* name,
        BufferMemoryAllocatorType mem_type = BufferMemoryAllocatorType::NORMAL_MALLOC,
        size_t alignment = 64
    );
    
    /**
     * @brief 将类型转换为字符串
     * 
     * @param type 类型
     * @return 类型名称
     */
    static const char* typeToString(AllocatorType type);

private:
    /**
     * @brief 根据类型创建Allocator
     * 
     * @param type Allocator类型
     * @param mem_type 内存分配器类型
     * @param alignment 内存对齐
     * @return Allocator实例
     */
    static std::unique_ptr<BufferAllocatorBase> createByType(
        AllocatorType type,
        BufferMemoryAllocatorType mem_type,
        size_t alignment
    );
};

#endif // BUFFER_ALLOCATOR_FACTORY_HPP

