#ifndef LINUX_FRAMEBUFFER_DEVICE_HPP
#define LINUX_FRAMEBUFFER_DEVICE_HPP

#include "IDisplayDevice.hpp"
#include "Buffer.hpp"
#include <vector>

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
    
    // ============ Buffer管理 ============
    std::vector<Buffer> buffers_;     // Buffer对象数组（动态分配，根据硬件实际数量）
    int current_buffer_index_;        // 当前显示的buffer索引
    
    // ============ 显示属性 ============
    int width_;                       // 显示宽度（像素）
    int height_;                      // 显示高度（像素）
    int bytes_per_pixel_;             // 每像素字节数
    size_t buffer_size_;              // 单个buffer大小（字节）
    
    // ============ 状态标志 ============
    bool is_initialized_;
    
    // ============ 内部辅助方法 ============
    
    /**
     * 初始化framebuffer信息
     * 通过ioctl读取分辨率、buffer数量等
     */
    bool initializeFramebufferInfo();
    
    /**
     * 执行mmap映射
     * 将整个framebuffer映射到用户空间
     */
    bool mapFramebufferMemory();
    
    /**
     * 计算每个buffer的虚拟地址
     * buffer[i] = framebuffer_base + (buffer_size * i)
     */
    void calculateBufferAddresses();
    
    /**
     * 解除mmap映射
     */
    void unmapFramebufferMemory();

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
    int getBufferCount() const override;
    size_t getBufferSize() const override;
    
    Buffer& getBuffer(int buffer_index) override;
    const Buffer& getBuffer(int buffer_index) const override;
    
    bool displayBuffer(int buffer_index) override;
    bool waitVerticalSync() override;
    int getCurrentDisplayBuffer() const override;
};

#endif // LINUX_FRAMEBUFFER_DEVICE_HPP

