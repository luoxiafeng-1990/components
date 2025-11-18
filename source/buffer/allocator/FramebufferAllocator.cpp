#include "../../../include/buffer/allocator/FramebufferAllocator.hpp"
#include "../../../include/display/LinuxFramebufferDevice.hpp"
#include <stdio.h>

// ============================================================
// 构造/析构函数
// ============================================================

FramebufferAllocator::FramebufferAllocator(const std::vector<BufferInfo>& external_buffers)
    : external_buffers_(external_buffers)
    , next_buffer_index_(0)
{
    printf("🔧 FramebufferAllocator created with %zu external buffers\n", 
           external_buffers_.size());
}

FramebufferAllocator::FramebufferAllocator(LinuxFramebufferDevice* device)
    : next_buffer_index_(0)
{
    if (!device) {
        printf("❌ ERROR: Device pointer is null\n");
        return;
    }
    
    // 调用私有方法构建 BufferInfo 列表
    external_buffers_ = buildBufferInfosFromDevice(device);
    
    printf("🔧 FramebufferAllocator created from device with %zu buffers\n", 
           external_buffers_.size());
}

FramebufferAllocator::~FramebufferAllocator() {
    printf("🧹 FramebufferAllocator destroyed (external memory not freed)\n");
}

// ============================================================
// 重写批量分配（批量包装）
// ============================================================

std::unique_ptr<BufferPool> FramebufferAllocator::allocatePoolWithBuffers(
    int count,
    size_t size,
    const std::string& name,
    const std::string& category)
{
    // 忽略 count 和 size，使用 external_buffers_ 的实际数量
    int actual_count = static_cast<int>(external_buffers_.size());
    
    printf("\n🏭 FramebufferAllocator: Wrapping %d external buffers to pool '%s'...\n",
           actual_count, name.c_str());
    
    // 1. 创建空池
    auto pool = BufferPool::CreateEmpty(name, category);
    if (!pool) {
        printf("❌ Failed to create empty pool\n");
        return nullptr;
    }
    
    // 2. 批量包装外部 Buffer
    for (int i = 0; i < actual_count; i++) {
        Buffer* buffer = createBuffer(i, 0);  // size 参数被忽略
        if (!buffer) {
            printf("❌ Failed to wrap external buffer #%d\n", i);
            // 失败：清理已创建的 buffer 对象
            cleanupPool(pool.get());
            return nullptr;
        }
        
        // 3. 通过基类辅助方法添加到 pool 的 free 队列
        if (!addBufferToPoolQueue(pool.get(), buffer, QueueType::FREE)) {
            printf("❌ Failed to add buffer #%d to pool\n", i);
            deallocateBuffer(buffer);
            cleanupPool(pool.get());
            return nullptr;
        }
        
        // 4. 记录所有权
        registerBufferOwnership(buffer, this);
        
        printf("   ✅ Buffer #%d wrapped: virt=%p, phys=0x%lx, size=%zu (EXTERNAL)\n",
               i, buffer->getVirtualAddress(), buffer->getPhysicalAddress(), buffer->size());
    }
    
    printf("✅ BufferPool '%s' created with %d external buffers by FramebufferAllocator\n", 
           name.c_str(), actual_count);
    
    return pool;
}

// ============================================================
// 核心实现
// ============================================================

Buffer* FramebufferAllocator::createBuffer(uint32_t id, size_t size) {
    // 检查 id 是否越界
    if (id >= external_buffers_.size()) {
        printf("❌ Buffer ID %u out of range (max: %zu)\n", 
               id, external_buffers_.size());
        return nullptr;
    }
    
    // 获取外部 buffer 信息
    const BufferInfo& info = external_buffers_[id];
    
    // 创建 Buffer 对象（Ownership::EXTERNAL）
    Buffer* buffer = new Buffer(
        id,
        info.virt_addr,
        info.phys_addr,
        info.size,
        Buffer::Ownership::EXTERNAL
    );
    
    if (!buffer) {
        printf("❌ Failed to create Buffer object #%u\n", id);
        return nullptr;
    }
    
    return buffer;
}

void FramebufferAllocator::deallocateBuffer(Buffer* buffer) {
    if (!buffer) {
        return;
    }
    
    // 1. 不释放内存（外部管理）
    printf("   🗑️ Deleting Buffer #%u (external memory retained)\n", buffer->id());
    
    // 2. 仅删除 Buffer 对象
    delete buffer;
}

// ============================================================
// 私有辅助方法：从设备构建 BufferInfo 列表
// ============================================================

std::vector<FramebufferAllocator::BufferInfo> 
FramebufferAllocator::buildBufferInfosFromDevice(LinuxFramebufferDevice* device)
{
    std::vector<BufferInfo> infos;
    
    if (!device) {
        printf("❌ ERROR: Device pointer is null in buildBufferInfosFromDevice\n");
        return infos;
    }
    
    // 1. 从设备获取 mmap 信息
    auto mapped_info = device->getMappedInfo();
    
    printf("📋 Building BufferInfo list from device:\n");
    printf("   base_addr=%p, buffer_size=%zu, buffer_count=%d\n",
           mapped_info.base_addr, mapped_info.buffer_size, mapped_info.buffer_count);
    
    // 2. 计算每个 buffer 的地址并构建 BufferInfo
    unsigned char* base = (unsigned char*)mapped_info.base_addr;
    infos.reserve(mapped_info.buffer_count);
    
    for (int i = 0; i < mapped_info.buffer_count; i++) {
        infos.push_back({
            .virt_addr = (void*)(base + i * mapped_info.buffer_size),
            .phys_addr = 0,  // 物理地址由系统自动获取
            .size = mapped_info.buffer_size
        });
        
        printf("   Buffer[%d]: virt=%p, size=%zu\n", 
               i, infos.back().virt_addr, infos.back().size);
    }
    
    return infos;
}

