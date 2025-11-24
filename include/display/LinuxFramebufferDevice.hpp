#ifndef LINUX_FRAMEBUFFER_DEVICE_HPP
#define LINUX_FRAMEBUFFER_DEVICE_HPP

#include "IDisplayDevice.hpp"
#include "../buffer/Buffer.hpp"
#include "../buffer/BufferPool.hpp"
#include "../buffer/allocator/facade/BufferAllocatorFacade.hpp"
#include "../buffer/allocator/factory/BufferAllocatorFactory.hpp"
#include <vector>
#include <memory>
#include <stdexcept>

/**
 * LinuxFramebufferDevice - Linux Framebuffer 显示设备实现
 * 
 * 实现基于 Linux Framebuffer 的显示功能，包括：
 * - 通过 /dev/fbX 设备节点访问framebuffer
 * - 使用 mmap 映射framebuffer到用户空间
 * - 通过 ioctl 进行显示控制（切换buffer、VSync等）
 * 
 * 支持多缓冲（通常4个buffer）：
 * - 虚拟framebuffer高度 = 物理高度 × buffer数量
 * - 通过设置yoffset切换不同的buffer
 */
class LinuxFramebufferDevice : public IDisplayDevice {
private:
    // ============ Linux特有资源 ============
    int fd_;                          // framebuffer文件描述符
    int fb_index_;                    // framebuffer索引（0或1）
    
    // ============ mmap映射管理 ============
    void* framebuffer_base_;          // mmap返回的基地址
    size_t framebuffer_total_size_;   // 映射的总大小
    
    // ============ Buffer管理（使用BufferPool）============
    std::unique_ptr<BufferAllocatorFacade> allocator_facade_;  // 门面类对象
    std::shared_ptr<BufferPool> buffer_pool_;                  // BufferPool（智能指针管理）
    std::vector<void*> fb_mappings_;          // framebuffer 映射地址（用于物理地址查询）
    int buffer_count_;                        // buffer 数量
    int current_buffer_index_;                // 当前显示的buffer索引
    
    // ============ 显示属性 ============
    int width_;                       // 显示宽度（像素）
    int height_;                      // 显示高度（像素）
    int bits_per_pixel_;              // 每像素位数（可以是非整数字节，如12bit、16bit、24bit、32bit等）
    size_t buffer_size_;              // 单个buffer大小（字节）
    
    // ============ 状态标志 ============
    bool is_initialized_;
    
    // ============ 内部辅助方法 ============
    
    /**
     * 查询硬件显示参数
     * 通过ioctl从硬件读取分辨率、bits_per_pixel、buffer数量等显示参数
     */
    bool queryHardwareDisplayParameters();
    
    /**
     * 执行mmap映射
     * 将整个硬件framebuffer内存映射到用户空间
     */
    bool mapHardwareFramebufferMemory();
    
    /**
     * 计算每个buffer的虚拟地址
     * buffer[i] = framebuffer_base + (buffer_size * i)
     */
    void calculateBufferAddresses();
    
    /**
     * 解除硬件framebuffer内存的mmap映射
     */
    void unmapHardwareFramebufferMemory();

public:
    LinuxFramebufferDevice();
    ~LinuxFramebufferDevice() override;
    
    // ============ IDisplayDevice接口实现 ============
    
    const char* findDeviceNode(int device_index) override;
    
    bool initialize(int device_index) override;
    void cleanup() override;
    
    int getWidth() const override;
    int getHeight() const override;
    int getBytesPerPixel() const override;
    int getBitsPerPixel() const override;
    int getBufferCount() const override;
    size_t getBufferSize() const override;
    
    bool displayBuffer(Buffer* buffer) override;
    bool displayBuffer(BufferPool* pool, int buffer_index) override;
    bool waitVerticalSync() override;
    int getCurrentDisplayBuffer() const override;
    
    // ============ 新接口：显式显示方法（按显示方式拆分）============
    
    /**
     * @brief 通过 DMA 零拷贝方式显示 buffer
     * @param buffer 带有物理地址的 Buffer 对象
     * @return true 显示成功，false 显示失败
     * 
     * @note 要求：
     *   - buffer 必须有有效的物理地址（phys_addr != 0）
     *   - 驱动必须支持 FB_IOCTL_SET_DMA_INFO ioctl
     * 
     * @note 优点：零拷贝，性能最高
     * @note 适用场景：视频解码器输出的 buffer（带物理地址）
     * 
     * @warning 如果 buffer 没有物理地址，此函数会失败
     */
    bool displayBufferByDMA(Buffer* buffer);
    
    /**
     * @brief 显示已填充的 framebuffer buffer
     * @param buffer 已填充数据的 framebuffer buffer 对象
     * @return true 显示成功，false 显示失败
     * 
     * @note 工作流程：
     *   1. 从 buffer 对象中获取 id（framebuffer 索引）
     *   2. 验证 buffer 是否属于当前 framebuffer 的 BufferPool
     *   3. 通过 ioctl 切换显示到该 buffer
     * 
     * @note 要求：
     *   - buffer 必须是从当前 framebuffer 的 BufferPool 获取的
     *   - buffer 必须已经填充了要显示的数据
     * 
     * @note 优点：无需拷贝，切换速度快，与 BufferPool 生产者模式配合
     * @note 适用场景：生产者填充 framebuffer buffer 后直接显示
     * 
     * 使用示例：
     * @code
     * BufferPool& pool = display.getBufferPool();
     * Buffer* fb_buf = pool.acquireFree(true, 1000);  // 生产者获取空闲 buffer
     * // ... 填充数据到 fb_buf ...
     * display.displayFilledFramebuffer(fb_buf);  // 显示填充后的 buffer
     * pool.releaseFilled(fb_buf);  // 归还到 filled 队列
     * @endcode
     */
    bool displayFilledFramebuffer(Buffer* buffer);
    
    /**
     * @brief 通过 memcpy 拷贝到 framebuffer 再显示
     * @param buffer 任意来源的 Buffer 对象
     * @return true 显示成功，false 显示失败
     * 
     * @note 工作流程：
     *   1. 从 framebuffer pool 获取一个空闲 buffer
     *   2. 将 buffer 的数据拷贝到 framebuffer buffer
     *   3. 切换显示到该 framebuffer buffer
     * 
     * @note 优点：通用性强，适用于任意来源的 buffer
     * @note 缺点：需要一次 memcpy，性能较低
     * @note 适用场景：来自网络/文件的 buffer，没有物理地址
     * 
     * @warning 性能开销大，仅在无法使用其他方法时使用
     */
    bool displayBufferByMemcpyToFramebuffer(Buffer* buffer);
    
    // ============ 新接口：信息提供和依赖注入 ============
    
    /**
     * @brief 获取 mmap 信息（供 FramebufferAllocator 使用）
     * 
     * @return MappedInfo 包含 mmap 后的内存信息
     * 
     * @note 必须在 initialize() 之后调用
     * 
     * 使用场景：
     * - FramebufferAllocator 需要这些信息来创建 BufferPool
     * 
     * 使用示例：
     * @code
     * auto device = std::make_unique<LinuxFramebufferDevice>();
     * device->initialize(0);
     * 
     * auto info = device->getMappedInfo();
     * // info.base_addr, info.buffer_size, info.buffer_count
     * @endcode
     */
    struct MappedInfo {
        void* base_addr;        // mmap 后的基地址
        size_t buffer_size;     // 单个 buffer 大小
        int buffer_count;       // buffer 数量
    };
    MappedInfo getMappedInfo() const;
    
    /**
     * @brief 获取 framebuffer 索引
     * 
     * @return int framebuffer 索引（0 或 1）
     */
    int getFbIndex() const { return fb_index_; }
    
    /**
     * @brief 获取当前的 BufferPool
     * 
     * @return BufferPool* BufferPool 指针（可能为 nullptr）
     * 
     * @note BufferPool 在 initialize() 中自动创建，无需手动设置
     */
    BufferPool* getBufferPool() const { return buffer_pool_.get(); }
};

#endif // LINUX_FRAMEBUFFER_DEVICE_HPP
