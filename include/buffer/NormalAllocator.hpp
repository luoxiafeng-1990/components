#pragma once

#include "BufferAllocator.hpp"
#include <cstdlib>

/**
 * @brief NormalAllocator - 普通内存分配器
 * 
 * 使用标准 C++ 内存分配（malloc/posix_memalign）创建 Buffer
 * 
 * 特点：
 * - 虚拟内存：是
 * - 物理地址：否（phys_addr = 0）
 * - 连续性：不保证物理连续
 * 
 * 使用场景：
 * - CPU 处理的普通数据缓冲
 * - 不需要 DMA 访问的场景
 * 
 * 使用示例：
 * @code
 * auto allocator = std::make_unique<NormalAllocator>(BufferMemoryAllocatorType::NORMAL_MALLOC);
 * auto pool = allocator->allocatePoolWithBuffers(10, 1920*1080*3, "VideoPool", "Video");
 * @endcode
 */
class NormalAllocator : public BufferAllocator {
public:
    /**
     * @brief 构造函数
     * 
     * @param type 分配器类型（通常为 NORMAL_MALLOC）
     * @param alignment 内存对齐（默认 64 字节）
     */
    explicit NormalAllocator(
        BufferMemoryAllocatorType type = BufferMemoryAllocatorType::NORMAL_MALLOC,
        size_t alignment = 64
    );
    
    ~NormalAllocator() override;
    
protected:
    /**
     * @brief 创建单个 Buffer（分配内存）
     * 
     * 实现逻辑：
     * 1. 使用 posix_memalign 分配对齐内存
     * 2. 创建 Buffer 对象（phys_addr = 0）
     * 3. 返回 Buffer 指针
     * 
     * @param id Buffer ID
     * @param size Buffer 大小
     * @return Buffer* 成功返回 buffer，失败返回 nullptr
     */
    Buffer* createBuffer(uint32_t id, size_t size) override;
    
    /**
     * @brief 销毁 Buffer（释放内存）
     * 
     * 实现逻辑：
     * 1. 释放 buffer 的内存（free）
     * 2. 删除 Buffer 对象
     * 
     * @param buffer 要销毁的 Buffer
     */
    void deallocateBuffer(Buffer* buffer) override;
    
private:
    BufferMemoryAllocatorType type_;
    size_t alignment_;
};

