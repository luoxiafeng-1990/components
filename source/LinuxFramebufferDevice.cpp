#include "../include/LinuxFramebufferDevice.hpp"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <string.h>
#include <errno.h>

// Framebuffer相关定义（参考原代码）
#define PROC_FB "/proc/fb"
#define TPS_FB0 "tpsfb0"
#define TPS_FB1 "tpsfb1"
#define DEV_FB0 "/dev/fb0"
#define DEV_FB1 "/dev/fb1"
#define DEV_FB2 "/dev/fb2"

// ============ 构造函数 ============

LinuxFramebufferDevice::LinuxFramebufferDevice()
    : fd_(-1)
    , fb_index_(-1)
    , framebuffer_base_(nullptr)
    , framebuffer_total_size_(0)
    , buffers_()  // vector自动初始化为空
    , current_buffer_index_(0)
    , width_(0)
    , height_(0)
    , bytes_per_pixel_(0)
    , buffer_size_(0)
    , is_initialized_(false)
{
    // vector会自动管理Buffer对象的生命周期
}

LinuxFramebufferDevice::~LinuxFramebufferDevice() {
    cleanup();
}

// ============ 公共接口实现 ============

bool LinuxFramebufferDevice::initialize(int device_index) {
    if (is_initialized_) {
        printf("⚠️  Warning: Device already initialized\n");
        return true;
    }
    
    fb_index_ = device_index;
    
    // 1. 查找framebuffer设备节点
    const char* device_node = findDeviceNode(fb_index_);
    if (!device_node) {
        printf("❌ ERROR: Cannot find framebuffer device for fb%d\n", fb_index_);
        return false;
    }
    
    printf("📂 Found framebuffer device: %s\n", device_node);
    
    // 2. 打开framebuffer设备
    fd_ = open(device_node, O_RDWR);
    if (fd_ < 0) {
        printf("❌ ERROR: Cannot open %s: %s\n", device_node, strerror(errno));
        return false;
    }
    
    // 3. 初始化framebuffer信息
    if (!initializeFramebufferInfo()) {
        close(fd_);
        fd_ = -1;
        return false;
    }
    
    // 4. mmap映射framebuffer内存
    if (!mapFramebufferMemory()) {
        close(fd_);
        fd_ = -1;
        return false;
    }
    
    // 5. 计算每个buffer的虚拟地址并创建Buffer对象
    calculateBufferAddresses();
    
    is_initialized_ = true;
    current_buffer_index_ = 0;
    
    // 打印初始化成功的总结信息
    printf("✅ Display initialized: %dx%d, %d buffers, %d bytes/pixel\n",
           width_, height_, static_cast<int>(buffers_.size()), bytes_per_pixel_);
    
    return true;
}

void LinuxFramebufferDevice::cleanup() {
    if (!is_initialized_) {
        return;
    }
    
    // 1. 解除内存映射
    unmapFramebufferMemory();
    
    // 2. 关闭文件描述符
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    
    // 3. 重置状态
    is_initialized_ = false;
    current_buffer_index_ = 0;
    
    // 清空Buffer对象（vector自动释放内存）
    buffers_.clear();
    
    printf("✅ LinuxFramebufferDevice cleaned up\n");
}

int LinuxFramebufferDevice::getWidth() const {
    return width_;
}

int LinuxFramebufferDevice::getHeight() const {
    return height_;
}

int LinuxFramebufferDevice::getBytesPerPixel() const {
    return bytes_per_pixel_;
}

int LinuxFramebufferDevice::getBufferCount() const {
    return static_cast<int>(buffers_.size());
}

size_t LinuxFramebufferDevice::getBufferSize() const {
    return buffer_size_;
}

Buffer& LinuxFramebufferDevice::getBuffer(int buffer_index) {
    if (buffer_index < 0 || buffer_index >= static_cast<int>(buffers_.size())) {
        static Buffer invalid_buffer;
        printf("❌ ERROR: Invalid buffer index %d (valid range: 0-%d)\n", 
               buffer_index, static_cast<int>(buffers_.size()) - 1);
        return invalid_buffer;
    }
    
    return buffers_[buffer_index];
}

const Buffer& LinuxFramebufferDevice::getBuffer(int buffer_index) const {
    if (buffer_index < 0 || buffer_index >= static_cast<int>(buffers_.size())) {
        static Buffer invalid_buffer;
        printf("❌ ERROR: Invalid buffer index %d (valid range: 0-%d)\n", 
               buffer_index, static_cast<int>(buffers_.size()) - 1);
        return invalid_buffer;
    }
    
    return buffers_[buffer_index];
}

bool LinuxFramebufferDevice::displayBuffer(int buffer_index) {
    if (!is_initialized_) {
        printf("❌ ERROR: Device not initialized\n");
        return false;
    }
    
    if (buffer_index < 0 || buffer_index >= static_cast<int>(buffers_.size())) {
        printf("❌ ERROR: Invalid buffer index %d\n", buffer_index);
        return false;
    }
    
    // 获取当前屏幕信息
    struct fb_var_screeninfo var_info;
    if (ioctl(fd_, FBIOGET_VSCREENINFO, &var_info) < 0) {
        printf("❌ ERROR: FBIOGET_VSCREENINFO failed: %s\n", strerror(errno));
        return false;
    }
    
    // 设置yoffset（buffer索引 * 屏幕高度）
    // 这样驱动就知道从哪个buffer读取数据显示
    var_info.yoffset = var_info.yres * buffer_index;
    
    // 通过ioctl通知驱动切换buffer
    if (ioctl(fd_, FBIOPAN_DISPLAY, &var_info) < 0) {
        printf("❌ ERROR: FBIOPAN_DISPLAY failed: %s\n", strerror(errno));
        return false;
    }
    
    current_buffer_index_ = buffer_index;
    return true;
}

bool LinuxFramebufferDevice::waitVerticalSync() {
    if (!is_initialized_) {
        printf("❌ ERROR: Device not initialized\n");
        return false;
    }
    
    int zero = 0;
    if (ioctl(fd_, FBIO_WAITFORVSYNC, &zero) < 0) {
        printf("⚠️  Warning: FBIO_WAITFORVSYNC failed: %s\n", strerror(errno));
        return false;
    }
    
    return true;
}

int LinuxFramebufferDevice::getCurrentDisplayBuffer() const {
    return current_buffer_index_;
}

// ============ 内部辅助方法实现 ============

const char* LinuxFramebufferDevice::findDeviceNode(int device_index) {
    FILE* fp;
    char line[256];
    int fb_num;
    char fb_name[32];
    
    // 打开/proc/fb文件
    fp = fopen(PROC_FB, "r");
    if (fp == NULL) {
        printf("❌ ERROR: Cannot open %s: %s\n", PROC_FB, strerror(errno));
        return NULL;
    }
    
    // 逐行读取/proc/fb内容，查找tpsfb0或tpsfb1
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "%d %s", &fb_num, fb_name) == 2) {
            const char* fb_str = device_index ? TPS_FB1 : TPS_FB0;
            if (strcmp(fb_name, fb_str) == 0) {
                fclose(fp);
                
                // 根据fb_num返回对应的设备节点
                if (fb_num == 0) {
                    return DEV_FB0;
                } else if (fb_num == 1) {
                    return DEV_FB1;
                } else if (fb_num == 2) {
                    return DEV_FB2;
                } else {
                    return NULL;
                }
            }
        }
    }
    
    fclose(fp);
    printf("❌ ERROR: %s not found in %s\n", 
           (device_index == 0) ? TPS_FB0 : TPS_FB1, PROC_FB);
    return NULL;
}

bool LinuxFramebufferDevice::initializeFramebufferInfo() {
    // 获取屏幕信息
    struct fb_var_screeninfo var_info;
    if (ioctl(fd_, FBIOGET_VSCREENINFO, &var_info) < 0) {
        printf("❌ ERROR: FBIOGET_VSCREENINFO failed: %s\n", strerror(errno));
        return false;
    }
    
    // 保存显示属性
    width_ = var_info.xres;
    height_ = var_info.yres;
    bytes_per_pixel_ = var_info.bits_per_pixel / 8;
    buffer_size_ = width_ * height_ * bytes_per_pixel_;
    
    // 计算buffer数量（虚拟高度 / 实际高度）
    int buffer_count = var_info.yres_virtual / var_info.yres;
    
    printf("📊 Framebuffer info:\n");
    printf("   xres=%d, yres=%d, bits_per_pixel=%d\n", 
           var_info.xres, var_info.yres, var_info.bits_per_pixel);
    printf("   yres_virtual=%d, buffer_count=%d\n", 
           var_info.yres_virtual, buffer_count);
    
    // 根据硬件实际的buffer数量动态分配Buffer对象
    buffers_.resize(buffer_count);
    printf("✅ Allocated %d Buffer objects\n", buffer_count);
    
    return true;
}

bool LinuxFramebufferDevice::mapFramebufferMemory() {
    // 计算需要映射的总大小
    framebuffer_total_size_ = buffer_size_ * buffers_.size();
    
    printf("🗺️  Mapping framebuffer: size=%zu bytes\n", framebuffer_total_size_);
    
    // 执行mmap映射
    framebuffer_base_ = mmap(0, framebuffer_total_size_,
                            PROT_READ | PROT_WRITE,
                            MAP_SHARED,
                            fd_,
                            0);
    
    if (framebuffer_base_ == MAP_FAILED) {
        printf("❌ ERROR: mmap failed: %s\n", strerror(errno));
        framebuffer_base_ = nullptr;
        return false;
    }
    
    printf("✅ mmap successful: base_address=%p\n", framebuffer_base_);
    
    return true;
}

void LinuxFramebufferDevice::calculateBufferAddresses() {
    unsigned char* base = (unsigned char*)framebuffer_base_;
    
    // 检查并调整到安全的 buffer 数量
    size_t required_size = buffer_size_ * buffers_.size();
    if (required_size > framebuffer_total_size_) {
        int safe_count = framebuffer_total_size_ / buffer_size_;
        printf("⚠️  WARNING: Adjusted buffer_count from %zu to %d (max safe value)\n", 
               buffers_.size(), safe_count);
        
        if (safe_count <= 0) {
            printf("❌ ERROR: Cannot fit even one buffer in mapped memory!\n");
            buffers_.clear();
            return;
        }
        
        // 调整 vector 大小到安全数量
        buffers_.resize(safe_count);
    }
    
    // 计算每个 buffer 的地址
    for (size_t i = 0; i < buffers_.size(); i++) {
        void* buffer_addr = (void*)(base + buffer_size_ * i);
        buffers_[i] = Buffer(buffer_addr, buffer_size_);
        
        printf("   Buffer[%zu]: address=%p, size=%zu\n", 
               i, buffers_[i].data(), buffers_[i].size());
    }
}

void LinuxFramebufferDevice::unmapFramebufferMemory() {
    if (framebuffer_base_ != nullptr) {
        if (munmap(framebuffer_base_, framebuffer_total_size_) < 0) {
            printf("⚠️  Warning: munmap failed: %s\n", strerror(errno));
        }
        framebuffer_base_ = nullptr;
        framebuffer_total_size_ = 0;
    }
}

