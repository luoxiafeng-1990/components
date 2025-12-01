#include "display/LinuxFramebufferDevice.hpp"
#include "buffer/allocator/facade/BufferAllocatorFacade.hpp"
#include "buffer/allocator/factory/BufferAllocatorFactory.hpp"
#include "buffer/allocator/implementation/FramebufferAllocator.hpp"
#include "buffer/BufferPoolRegistry.hpp"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <string.h>
#include <string>
#include <errno.h>
#include <stdint.h>
#include <vector>

// Framebuffer相关定义（参考原代码）
#define PROC_FB "/proc/fb"
#define TPS_FB0 "tpsfb0"
#define TPS_FB1 "tpsfb1"
#define DEV_FB0 "/dev/fb0"
#define DEV_FB1 "/dev/fb1"
#define DEV_FB2 "/dev/fb2"

// ============ 零拷贝 DMA 配置结构体和 ioctl ============
// 参考 taco-vo/core/taco_vo_layer.c:29-33 和 ids_test.cpp
struct tpsfb_dma_info {
    uint32_t ovl_idx;      // overlay 索引
    uint64_t phys_addr;    // 物理地址
};
#define FB_IOCTL_SET_DMA_INFO _IOW('F', 7, struct tpsfb_dma_info)

// ============ 构造函数 ============

LinuxFramebufferDevice::LinuxFramebufferDevice()
    : fd_(-1)
    , fb_index_(-1)
    , framebuffer_base_ptr_(nullptr)
    , framebuffer_total_size_(0)
    , allocator_facade_(nullptr)
    , buffer_pool_id_(0)  // v2.0: 在 initialize() 中自动创建并注册
    , buffer_count_(0)
    , current_buffer_index_(0)
    , width_(0)
    , height_(0)
    , bits_per_pixel_(0)
    , buffer_size_(0)
    , is_initialized_(false)
{
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
    
    // 5. 创建 allocator_facade_（通过 Factory 创建 FRAMEBUFFER 类型）
    allocator_facade_ = std::make_unique<BufferAllocatorFacade>(
        BufferAllocatorFactory::AllocatorType::FRAMEBUFFER
    );
    if (!allocator_facade_) {
        printf("❌ ERROR: Failed to create allocator_facade_\n");
        munmap(framebuffer_base_ptr_, framebuffer_total_size_);
        close(fd_);
        fd_ = -1;
        return false;
    }
    printf("✅ allocator_facade_ created for FRAMEBUFFER type\n");
    
    // 6. 通过 allocator 创建空的 BufferPool（v2.0: 返回 pool_id）
    std::string pool_name = "LinuxFramebufferDevice_fb" + std::to_string(fb_index_);
    buffer_pool_id_ = allocator_facade_->allocatePoolWithBuffers(
        0,  // count = 0，创建空 pool（稍后动态注入）
        0,  // size = 0，不使用
        pool_name,
        "Display"
    );
    
    if (buffer_pool_id_ == 0) {
        printf("❌ ERROR: Failed to create BufferPool through allocator\n");
        allocator_facade_.reset();
        munmap(framebuffer_base_ptr_, framebuffer_total_size_);
        close(fd_);
        fd_ = -1;
        return false;
    }
    
    // v2.0: 从 Registry 获取 Pool 以获取名称
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(buffer_pool_id_);
    auto pool = pool_weak.lock();
    if (pool) {
        printf("✅ Empty BufferPool '%s' created (ID: %lu)\n", pool->getName().c_str(), buffer_pool_id_);
    } else {
        printf("✅ Empty BufferPool created (ID: %lu)\n", buffer_pool_id_);
    }
    
    // 7. 动态注入 framebuffer buffers 到 BufferPool（v2.0: 使用 pool_id）
    unsigned char* base = (unsigned char*)framebuffer_base_ptr_;
    for (int i = 0; i < buffer_count_; i++) {
        void* virt_addr = (void*)(base + buffer_size_ * i);
        uint64_t phys_addr = 0;  // TODO: 获取实际物理地址
        
        Buffer* buffer = allocator_facade_->injectExternalBufferToPool(
            buffer_pool_id_,  // v2.0: 第一个参数是 pool_id
            virt_addr,        // v2.0: 第二个参数是 virt_addr
            phys_addr,        // v2.0: 第三个参数是 phys_addr
            buffer_size_,     // v2.0: 第四个参数是 size
            QueueType::FREE
        );
        
        if (!buffer) {
            printf("❌ ERROR: Failed to inject buffer #%d to BufferPool\n", i);
            buffer_pool_id_ = 0;
            allocator_facade_.reset();
            munmap(framebuffer_base_ptr_, framebuffer_total_size_);
            close(fd_);
            fd_ = -1;
            return false;
        }
    }
    
    // v2.0: 从 Registry 获取 Pool 以获取名称
    pool = pool_weak.lock();
    if (pool) {
        printf("✅ All %d framebuffer buffers injected to BufferPool '%s'\n",
               buffer_count_, pool->getName().c_str());
    } else {
        printf("✅ All %d framebuffer buffers injected to BufferPool (ID: %lu)\n",
               buffer_count_, buffer_pool_id_);
    }
    
    is_initialized_ = true;
    current_buffer_index_ = 0;
    
    // 打印初始化成功的总结信息
    pool = pool_weak.lock();
    if (pool) {
        printf("✅ Display initialized: %dx%d, %d buffers, %d bits/pixel\n",
               width_, height_, pool->getTotalCount(), bits_per_pixel_);
    } else {
        printf("✅ Display initialized: %dx%d, %d buffers, %d bits/pixel\n",
               width_, height_, buffer_count_, bits_per_pixel_);
    }
    
    return true;
}

void LinuxFramebufferDevice::cleanup() {
    if (!is_initialized_) {
        return;
    }
    
    // v2.0: 重置 pool_id（BufferPool 的生命周期由 Registry 和 Allocator 管理）
    buffer_pool_id_ = 0;
    
    // 2. 重置 allocator_facade_（会自动销毁底层 allocator 和 BufferPool）
    allocator_facade_.reset();
    
    // 3. 解除硬件framebuffer内存映射
    unmapHardwareFramebufferMemory();
    
    // 4. 关闭文件描述符
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    
    // 5. 重置状态
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
    if (buffer_pool_id_ != 0) {
        auto pool_weak = BufferPoolRegistry::getInstance().getPool(buffer_pool_id_);
        if (auto pool = pool_weak.lock()) {
            return pool->getTotalCount();
        }
    }
    return buffer_count_;  // 返回硬件 buffer 数量
}

size_t LinuxFramebufferDevice::getBufferSize() const {
    return buffer_size_;
}

bool LinuxFramebufferDevice::displayBuffer(Buffer* buffer) {
    if (!is_initialized_) {
        printf("❌ ERROR: Device not initialized\n");
        return false;
    }
    
    if (!buffer) {
        printf("❌ ERROR: Null buffer pointer\n");
        return false;
    }
    
    if (buffer_pool_id_ == 0) {
        printf("❌ ERROR: BufferPool not initialized\n");
        return false;
    }
    
    // v2.0: 从 Registry 获取 Pool
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(buffer_pool_id_);
    auto pool = pool_weak.lock();
    if (!pool) {
        printf("❌ ERROR: BufferPool (ID: %lu) not found or already destroyed\n", buffer_pool_id_);
        return false;
    }
    
    // 验证Buffer是否属于当前设备的BufferPool
    Buffer* pool_buffer = pool->getBufferById(buffer->id());
    if (!pool_buffer || pool_buffer != buffer) {
        printf("❌ ERROR: Buffer (ID=%u) does not belong to device's BufferPool\n", 
               buffer->id());
        return false;
    }
    
    // 通过Buffer的ID获取buffer_index
    int buffer_index = static_cast<int>(buffer->id());
    
    if (buffer_index < 0 || buffer_index >= buffer_count_) {
        printf("❌ ERROR: Invalid buffer index %d (from Buffer ID %u)\n", 
               buffer_index, buffer->id());
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

bool LinuxFramebufferDevice::displayBuffer(BufferPool* pool, int buffer_index) {
    if (!is_initialized_) {
        printf("❌ ERROR: Device not initialized\n");
        return false;
    }
    
    if (!pool) {
        printf("❌ ERROR: Null BufferPool pointer\n");
        return false;
    }
    
    // v2.0: 验证BufferPool是否是当前设备的BufferPool
    if (buffer_pool_id_ != 0) {
        auto pool_weak = BufferPoolRegistry::getInstance().getPool(buffer_pool_id_);
        auto device_pool = pool_weak.lock();
        if (device_pool && pool != device_pool.get()) {
            printf("⚠️  Warning: BufferPool mismatch (provided pool != device's buffer_pool_)\n");
            printf("   Continuing anyway...\n");
        }
    }
    
    if (buffer_index < 0 || buffer_index >= buffer_count_) {
        printf("❌ ERROR: Invalid buffer index %d (valid range: 0-%d)\n", 
               buffer_index, buffer_count_ - 1);
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
    framebuffer_base_ptr_ = mmap(0, framebuffer_total_size_,
                            PROT_READ | PROT_WRITE,
                            MAP_SHARED,
                            fd_,
                            0);
    
    if (framebuffer_base_ptr_ == MAP_FAILED) {
        printf("❌ ERROR: mmap failed: %s\n", strerror(errno));
        framebuffer_base_ptr_ = nullptr;
        return false;
    }
    
    printf("✅ mmap successful: base_address=%p\n", framebuffer_base_ptr_);
    
    return true;
}


void LinuxFramebufferDevice::unmapHardwareFramebufferMemory() {
    if (framebuffer_base_ptr_ != nullptr) {
        if (munmap(framebuffer_base_ptr_, framebuffer_total_size_) < 0) {
            printf("⚠️  Warning: munmap failed: %s\n", strerror(errno));
        }
        framebuffer_base_ptr_ = nullptr;
        framebuffer_total_size_ = 0;
    }
}

// ============ 新接口实现：信息提供和依赖注入 ============

LinuxFramebufferDevice::MappedInfo LinuxFramebufferDevice::getMappedInfo() const {
    MappedInfo info;
    info.base_addr = framebuffer_base_ptr_;
    info.buffer_size = buffer_size_;
    info.buffer_count = buffer_count_;
    return info;
}

// ============ 新接口：displayBuffer(Buffer*) - 智能零拷贝显示 ============

// ========================================
// 显式显示方法（按显示方式拆分）
// ========================================

bool LinuxFramebufferDevice::displayBufferByDMA(Buffer* buffer) {
    if (!is_initialized_) {
        printf("❌ ERROR: Device not initialized\n");
        return false;
    }
    
    if (!buffer) {
        printf("❌ ERROR: Null buffer pointer\n");
        return false;
    }
    
    // 检查是否有物理地址
    uint64_t phys_addr = buffer->getPhysicalAddress();
    if (phys_addr == 0) {
        printf("❌ ERROR: Buffer has no physical address (phys_addr=0)\n");
        printf("   Hint: DMA display requires buffer with physical address\n");
        return false;
    }
    
    // 静态计数器，用于日志节流（避免过度打印）
    static int display_count = 0;
    
    // 设置 DMA 信息
    struct tpsfb_dma_info dma_info;
    dma_info.ovl_idx = 0;  // overlay 0
    dma_info.phys_addr = phys_addr;
    
    // 设置 DMA 物理地址
    if (ioctl(fd_, FB_IOCTL_SET_DMA_INFO, &dma_info) < 0) {
        printf("❌ ERROR: FB_IOCTL_SET_DMA_INFO failed: %s (phys_addr=0x%llx)\n", 
               strerror(errno), (unsigned long long)phys_addr);
        printf("   Hint: Driver may not support DMA display\n");
        return false;
    }
    
    // 获取当前屏幕信息
    struct fb_var_screeninfo var_info;
    if (ioctl(fd_, FBIOGET_VSCREENINFO, &var_info) < 0) {
        printf("❌ ERROR: FBIOGET_VSCREENINFO failed: %s\n", strerror(errno));
        return false;
    }
    
    // 关键：yoffset 设为 0，因为 DMA 直接从物理地址读取
    var_info.yoffset = 0;
    
    // 通知驱动显示（驱动会通过 DMA 从 phys_addr 读取数据）
    if (ioctl(fd_, FBIOPAN_DISPLAY, &var_info) < 0) {
        printf("❌ ERROR: FBIOPAN_DISPLAY failed: %s\n", strerror(errno));
        return false;
    }
    
    // 统计和日志（每100帧打印一次）
    display_count++;
    if (display_count == 1 || display_count % 100 == 0) {
        printf("🚀 [DMA Display] Frame #%d (phys_addr=0x%llx, buffer_id=%u)\n",
               display_count, (unsigned long long)phys_addr, buffer->id());
    }
    
    current_buffer_index_ = 0;  // DMA 模式下固定为 0
    return true;
}

bool LinuxFramebufferDevice::displayFilledFramebuffer(Buffer* buffer) {
    if (!is_initialized_) {
        printf("❌ ERROR: Device not initialized\n");
        return false;
    }
    
    if (!buffer) {
        printf("❌ ERROR: Null buffer pointer\n");
        return false;
    }
    
    if (buffer_pool_id_ == 0) {
        printf("❌ ERROR: BufferPool not initialized\n");
        return false;
    }
    
    // v2.0: 从 Registry 获取 Pool
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(buffer_pool_id_);
    auto pool = pool_weak.lock();
    if (!pool) {
        printf("❌ ERROR: BufferPool (ID: %lu) not found or already destroyed\n", buffer_pool_id_);
        return false;
    }
    
    // 从 buffer 对象中解析出 framebuffer id
    uint32_t buffer_id = buffer->id();
    
    // 验证 buffer_id 在有效范围内
    if (buffer_id >= static_cast<uint32_t>(buffer_count_)) {
        printf("❌ ERROR: Invalid buffer id %u (valid range: 0-%d)\n", 
               buffer_id, buffer_count_ - 1);
        printf("   Hint: This buffer may not belong to this framebuffer's BufferPool\n");
        return false;
    }
    
    // 可选：验证这个 buffer 是否确实属于我们的 BufferPool
    Buffer* pool_buffer = pool->getBufferById(buffer_id);
    if (pool_buffer != buffer) {
        printf("❌ ERROR: Buffer (id=%u) does not belong to this framebuffer's BufferPool\n", 
               buffer_id);
        printf("   Buffer pointer: %p, Expected: %p\n", (void*)buffer, (void*)pool_buffer);
        return false;
    }
    
    // 静态计数器，用于日志节流
    static int display_count = 0;
    
    // 获取当前屏幕信息
    struct fb_var_screeninfo var_info;
    if (ioctl(fd_, FBIOGET_VSCREENINFO, &var_info) < 0) {
        printf("❌ ERROR: FBIOGET_VSCREENINFO failed: %s\n", strerror(errno));
        return false;
    }
    
    // 设置yoffset（buffer id * 屏幕高度）
    var_info.yoffset = var_info.yres * buffer_id;
    
    // 通过ioctl通知驱动切换buffer
    if (ioctl(fd_, FBIOPAN_DISPLAY, &var_info) < 0) {
        printf("❌ ERROR: FBIOPAN_DISPLAY failed: %s\n", strerror(errno));
        return false;
    }
    
    // 统计和日志
    display_count++;
    if (display_count == 1 || display_count % 100 == 0) {
        printf("🔄 [Framebuffer Switch] Frame #%d (buffer_id=%u)\n",
               display_count, buffer_id);
    }
    
    current_buffer_index_ = buffer_id;
    return true;
}

bool LinuxFramebufferDevice::displayBufferByMemcpyToFramebuffer(Buffer* buffer) {
    if (!is_initialized_) {
        printf("❌ ERROR: Device not initialized\n");
        return false;
    }
    
    if (!buffer) {
        printf("❌ ERROR: Null buffer pointer\n");
        return false;
    }
    
    if (buffer_pool_id_ == 0) {
        printf("❌ ERROR: BufferPool not initialized\n");
        return false;
    }
    
    // v2.0: 从 Registry 获取 Pool
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(buffer_pool_id_);
    auto pool = pool_weak.lock();
    if (!pool) {
        printf("❌ ERROR: BufferPool (ID: %lu) not found or already destroyed\n", buffer_pool_id_);
        return false;
    }
    
    // 静态计数器，用于日志节流
    static int display_count = 0;
    
    // 获取一个空闲的 framebuffer buffer 来接收数据
    Buffer* fb_buffer = pool->acquireFree(false, 0);  // 非阻塞获取
    if (!fb_buffer) {
        printf("❌ ERROR: No free framebuffer buffer available\n");
        printf("   Hint: All framebuffer buffers are busy, try again later\n");
        return false;
    }
    
    // 检查大小是否匹配
    if (buffer->size() != fb_buffer->size()) {
        printf("⚠️  Warning: Buffer size mismatch (%zu vs %zu), copying min size\n",
               buffer->size(), fb_buffer->size());
    }
    
    size_t copy_size = (buffer->size() < fb_buffer->size()) ? buffer->size() : fb_buffer->size();
    
    // 执行 memcpy
    memcpy(fb_buffer->getVirtualAddress(), 
           buffer->getVirtualAddress(), 
           copy_size);
    
    // 显示这个 framebuffer buffer
    uint32_t fb_buffer_id = fb_buffer->id();
    
    // 获取当前屏幕信息
    struct fb_var_screeninfo var_info;
    if (ioctl(fd_, FBIOGET_VSCREENINFO, &var_info) < 0) {
        printf("❌ ERROR: FBIOGET_VSCREENINFO failed: %s\n", strerror(errno));
        pool->releaseFilled(fb_buffer);  // 归还 buffer
        return false;
    }
    
    // 设置yoffset
    var_info.yoffset = var_info.yres * fb_buffer_id;
    
    // 通过ioctl通知驱动切换buffer
    if (ioctl(fd_, FBIOPAN_DISPLAY, &var_info) < 0) {
        printf("❌ ERROR: FBIOPAN_DISPLAY failed: %s\n", strerror(errno));
        pool->releaseFilled(fb_buffer);  // 归还 buffer
        return false;
    }
    
    // 统计和日志
    display_count++;
    if (display_count == 1 || display_count % 100 == 0) {
        printf("📋 [Memcpy Display] Frame #%d (copied %zu bytes to fb_buffer[%u])\n",
               display_count, copy_size, fb_buffer_id);
    }
    
    // 归还 framebuffer buffer 到 free_queue
    // 这是安全的，因为：
    // 1. 硬件会继续显示这个 buffer（直到下次切换）
    // 2. 有多个 framebuffer（通常4个），足够轮转
    pool->releaseFilled(fb_buffer);
    
    current_buffer_index_ = fb_buffer_id;
    return true;
}

