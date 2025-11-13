#include "../../include/display/LinuxFramebufferDevice.hpp"
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
    , buffer_pool_(nullptr)
    , buffer_count_(0)
    , current_buffer_index_(0)
    , width_(0)
    , height_(0)
    , bits_per_pixel_(0)
    , buffer_size_(0)
    , is_initialized_(false)
{
    // BufferPool 会在 initialize() 中创建
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
    
    // 3. 查询硬件显示参数
    if (!queryHardwareDisplayParameters()) {
        close(fd_);
        fd_ = -1;
        return false;
    }
    
    // 4. mmap映射硬件framebuffer内存
    if (!mapHardwareFramebufferMemory()) {
        close(fd_);
        fd_ = -1;
        return false;
    }
    
    // 5. 计算每个buffer的虚拟地址并创建Buffer对象
    calculateBufferAddresses();
    
    is_initialized_ = true;
    current_buffer_index_ = 0;
    
    // 打印初始化成功的总结信息
    printf("✅ Display initialized: %dx%d, %d buffers, %d bits/pixel\n",
           width_, height_, buffer_count_, bits_per_pixel_);
    
    return true;
}

void LinuxFramebufferDevice::cleanup() {
    if (!is_initialized_) {
        return;
    }
    
    // 1. 解除硬件framebuffer内存映射
    unmapHardwareFramebufferMemory();
    
    // 2. 关闭文件描述符
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    
    // 3. 重置 BufferPool
    buffer_pool_.reset();
    
    // 4. 重置状态
    is_initialized_ = false;
    current_buffer_index_ = 0;
    buffer_count_ = 0;
    
    printf("✅ LinuxFramebufferDevice cleaned up\n");
}

int LinuxFramebufferDevice::getWidth() const {
    return width_;
}

int LinuxFramebufferDevice::getHeight() const {
    return height_;
}

int LinuxFramebufferDevice::getBytesPerPixel() const {
    // 注意：这里返回的是向上取整的字节数
    // 例如：12bit -> 2字节，16bit -> 2字节，24bit -> 3字节
    // 实际使用时可能需要根据具体的像素格式进行处理
    return (bits_per_pixel_ + 7) / 8;
}

int LinuxFramebufferDevice::getBitsPerPixel() const {
    return bits_per_pixel_;
}

int LinuxFramebufferDevice::getBufferCount() const {
    if (buffer_pool_) {
        return buffer_pool_->getTotalCount();
    }
    return 0;
}

size_t LinuxFramebufferDevice::getBufferSize() const {
    return buffer_size_;
}

Buffer& LinuxFramebufferDevice::getBuffer(int buffer_index) {
    if (!buffer_pool_) {
        static Buffer invalid_buffer(0, nullptr, 0, 0, Buffer::Ownership::EXTERNAL);
        printf("❌ ERROR: BufferPool not initialized\n");
        return invalid_buffer;
    }
    
    Buffer* buf = buffer_pool_->getBufferById(buffer_index);
    if (!buf) {
        static Buffer invalid_buffer(0, nullptr, 0, 0, Buffer::Ownership::EXTERNAL);
        printf("❌ ERROR: Invalid buffer index %d (valid range: 0-%d)\n", 
               buffer_index, getBufferCount() - 1);
        return invalid_buffer;
    }
    
    return *buf;
}

const Buffer& LinuxFramebufferDevice::getBuffer(int buffer_index) const {
    if (!buffer_pool_) {
        static Buffer invalid_buffer(0, nullptr, 0, 0, Buffer::Ownership::EXTERNAL);
        printf("❌ ERROR: BufferPool not initialized\n");
        return invalid_buffer;
    }
    
    const Buffer* buf = buffer_pool_->getBufferById(buffer_index);
    if (!buf) {
        static Buffer invalid_buffer(0, nullptr, 0, 0, Buffer::Ownership::EXTERNAL);
        printf("❌ ERROR: Invalid buffer index %d (valid range: 0-%d)\n", 
               buffer_index, getBufferCount() - 1);
        return invalid_buffer;
    }
    
    return *buf;
}

bool LinuxFramebufferDevice::displayBuffer(int buffer_index) {
    if (!is_initialized_) {
        printf("❌ ERROR: Device not initialized\n");
        return false;
    }
    
    if (buffer_index < 0 || buffer_index >= buffer_count_) {
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

bool LinuxFramebufferDevice::queryHardwareDisplayParameters() {
    // 获取屏幕信息
    struct fb_var_screeninfo var_info;
    if (ioctl(fd_, FBIOGET_VSCREENINFO, &var_info) < 0) {
        printf("❌ ERROR: FBIOGET_VSCREENINFO failed: %s\n", strerror(errno));
        return false;
    }
    
    // 保存显示属性
    width_ = var_info.xres;
    height_ = var_info.yres;
    bits_per_pixel_ = var_info.bits_per_pixel;
    
    // 计算buffer大小：总位数 / 8 向上取整
    // 对于非整数字节的像素格式（如12bit），这样可以确保分配足够的内存
    size_t total_bits = static_cast<size_t>(width_) * height_ * bits_per_pixel_;
    buffer_size_ = (total_bits + 7) / 8;  // 向上取整到字节
    
    // 计算buffer数量（虚拟高度 / 实际高度）
    int buffer_count = var_info.yres_virtual / var_info.yres;
    
    printf("📊 Framebuffer info:\n");
    printf("   xres=%d, yres=%d, bits_per_pixel=%d\n", 
           var_info.xres, var_info.yres, var_info.bits_per_pixel);
    printf("   yres_virtual=%d, buffer_count=%d\n", 
           var_info.yres_virtual, buffer_count);
    
    // 保存 buffer 数量（稍后创建 BufferPool）
    buffer_count_ = buffer_count;
    printf("✅ Will create BufferPool with %d buffers\n", buffer_count_);
    
    return true;
}

bool LinuxFramebufferDevice::mapHardwareFramebufferMemory() {
    // 计算需要映射的总大小
    framebuffer_total_size_ = buffer_size_ * buffer_count_;
    
    printf("🗺️  Mapping framebuffer: size=%zu bytes (%d buffers × %zu bytes)\n", 
           framebuffer_total_size_, buffer_count_, buffer_size_);
    
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
    size_t required_size = buffer_size_ * buffer_count_;
    if (required_size > framebuffer_total_size_) {
        int safe_count = framebuffer_total_size_ / buffer_size_;
        printf("⚠️  WARNING: Adjusted buffer_count from %d to %d (max safe value)\n", 
               buffer_count_, safe_count);
        
        if (safe_count <= 0) {
            printf("❌ ERROR: Cannot fit even one buffer in mapped memory!\n");
            return;
        }
        
        buffer_count_ = safe_count;
    }
    
    // 计算每个 buffer 的地址并创建 BufferPool
    std::vector<BufferPool::ExternalBufferInfo> fb_infos;
    fb_mappings_.clear();
    fb_mappings_.reserve(buffer_count_);
    
    printf("🔧 Creating BufferPool with %d framebuffer buffers:\n", buffer_count_);
    
    for (int i = 0; i < buffer_count_; i++) {
        void* buffer_addr = (void*)(base + buffer_size_ * i);
        fb_mappings_.push_back(buffer_addr);
        
        // 尝试获取物理地址（可能失败，取决于权限）
        uint64_t phys_addr = 0;  // 暂时设为0，BufferPool会尝试自动获取
        
        fb_infos.push_back({
            .virt_addr = buffer_addr,
            .phys_addr = phys_addr,
            .size = buffer_size_
        });
        
        printf("   Framebuffer[%d]: virt=%p, size=%zu\n", 
               i, buffer_addr, buffer_size_);
    }
    
    // 创建 BufferPool（托管framebuffer）
    try {
        buffer_pool_ = std::make_unique<BufferPool>(fb_infos);
        printf("✅ BufferPool created successfully (managing %d framebuffers)\n", buffer_count_);
        buffer_pool_->printStats();
    } catch (const std::exception& e) {
        printf("❌ ERROR: Failed to create BufferPool: %s\n", e.what());
        buffer_pool_.reset();
    }
}

void LinuxFramebufferDevice::unmapHardwareFramebufferMemory() {
    if (framebuffer_base_ != nullptr) {
        if (munmap(framebuffer_base_, framebuffer_total_size_) < 0) {
            printf("⚠️  Warning: munmap failed: %s\n", strerror(errno));
        }
        framebuffer_base_ = nullptr;
        framebuffer_total_size_ = 0;
    }
}

// ============ 新接口：displayBuffer(Buffer*) ============

bool LinuxFramebufferDevice::displayBuffer(Buffer* buffer) {
    if (!is_initialized_) {
        printf("❌ ERROR: Device not initialized\n");
        return false;
    }
    
    if (!buffer) {
        printf("❌ ERROR: Null buffer pointer\n");
        return false;
    }
    
    if (!buffer_pool_) {
        printf("❌ ERROR: BufferPool not initialized\n");
        return false;
    }
    
    // 校验 buffer 是否属于这个 pool
    if (!buffer_pool_->validateBuffer(buffer)) {
        printf("❌ ERROR: Buffer validation failed (may not belong to this pool)\n");
        return false;
    }
    
    // 获取 buffer ID（即索引）
    uint32_t buffer_id = buffer->id();
    
    // 使用原有的 displayBuffer(int) 实现
    return displayBuffer(static_cast<int>(buffer_id));
}

