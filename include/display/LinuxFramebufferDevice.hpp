#ifndef LINUX_FRAMEBUFFER_DEVICE_HPP
#define LINUX_FRAMEBUFFER_DEVICE_HPP

#include "IDisplayDevice.hpp"
#include "../buffer/Buffer.hpp"
#include "../buffer/BufferPool.hpp"
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
    std::unique_ptr<BufferPool> buffer_pool_;  // BufferPool 管理 framebuffer
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
    
    Buffer& getBuffer(int buffer_index) override;
    const Buffer& getBuffer(int buffer_index) const override;
    
    bool displayBuffer(int buffer_index) override;
    bool waitVerticalSync() override;
    int getCurrentDisplayBuffer() const override;
    
    // ============ 新接口：BufferPool 访问 ============
    
    /**
     * @brief 获取 BufferPool 引用
     * @return BufferPool& 对内部 BufferPool 的引用
     * @throws std::runtime_error 如果 BufferPool 未初始化
     * 
     * @note 生命周期：返回的引用在 LinuxFramebufferDevice 对象存活期间有效
     * @warning 不要在 cleanup() 或析构后使用返回的引用
     * @warning 引用的生命周期与 LinuxFramebufferDevice 绑定
     * 
     * 使用示例：
     * @code
     * LinuxFramebufferDevice display;
     * display.initialize(0);
     * BufferPool& pool = display.getBufferPool();  // 注意是引用
     * VideoProducer producer(pool);
     * // 或直接传递：
     * VideoProducer producer2(display.getBufferPool());
     * @endcode
     */
    BufferPool& getBufferPool() {
        if (!buffer_pool_) {
            throw std::runtime_error("❌ BufferPool not initialized. Call initialize() first.");
        }
        return *buffer_pool_;
    }
    
    /**
     * @brief 获取 BufferPool 常量引用
     * @return const BufferPool& 对内部 BufferPool 的常量引用
     * @throws std::runtime_error 如果 BufferPool 未初始化
     */
    const BufferPool& getBufferPool() const {
        if (!buffer_pool_) {
            throw std::runtime_error("❌ BufferPool not initialized. Call initialize() first.");
        }
        return *buffer_pool_;
    }
    
    /**
     * @brief 显示指定的 buffer（使用新 Buffer 接口）
     * @param buffer Buffer 指针
     * @return true 如果显示成功
     */
    bool displayBuffer(Buffer* buffer);
};

#endif // LINUX_FRAMEBUFFER_DEVICE_HPP

