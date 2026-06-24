#include "../../include/buffer/BufferHandle.hpp"
#include <stdio.h>

// ========== 构造函数 ==========

BufferHandle::BufferHandle(void* virt_addr, 
                           uint64_t phys_addr,
                           size_t size,
                           Deleter deleter)
    : virt_addr_(virt_addr)
    , phys_addr_(phys_addr)
    , size_(size)
    , deleter_(deleter)
    , alive_(std::make_shared<bool>(true))  // 初始状态：存活
{
    printf("🔗 BufferHandle created: virt=%p, phys=0x%lx, size=%zu\n",
           virt_addr_, phys_addr_, size_);
}

// ========== 析构函数 ==========

BufferHandle::~BufferHandle() {
    if (virt_addr_ && alive_) {
        // 标记为已销毁
        *alive_ = false;
        
        // 调用自定义释放函数
        if (deleter_) {
            try {
                printf("🧹 BufferHandle destroying: %p (calling custom deleter)\n", virt_addr_);
                deleter_(virt_addr_);
            } catch (...) {
                printf("⚠️  Warning: Exception in BufferHandle deleter for %p\n", virt_addr_);
            }
        } else {
            printf("🧹 BufferHandle destroying: %p (no deleter)\n", virt_addr_);
        }
        
        virt_addr_ = nullptr;
    }
}

// ========== 移动构造函数 ==========

BufferHandle::BufferHandle(BufferHandle&& other) noexcept
    : virt_addr_(other.virt_addr_)
    , phys_addr_(other.phys_addr_)
    , size_(other.size_)
    , deleter_(std::move(other.deleter_))
    , alive_(std::move(other.alive_))
{
    // 清空被移动对象
    other.virt_addr_ = nullptr;
    other.phys_addr_ = 0;
    other.size_ = 0;
    other.alive_.reset();
}

// ========== 移动赋值运算符 ==========

BufferHandle& BufferHandle::operator=(BufferHandle&& other) noexcept {
    if (this != &other) {
        // 先释放当前资源
        if (virt_addr_ && alive_) {
            *alive_ = false;
            if (deleter_) {
                try {
                    deleter_(virt_addr_);
                } catch (...) {
                    printf("⚠️  Warning: Exception in BufferHandle deleter\n");
                }
            }
        }
        
        // 移动资源
        virt_addr_ = other.virt_addr_;
        phys_addr_ = other.phys_addr_;
        size_ = other.size_;
        deleter_ = std::move(other.deleter_);
        alive_ = std::move(other.alive_);
        
        // 清空被移动对象
        other.virt_addr_ = nullptr;
        other.phys_addr_ = 0;
        other.size_ = 0;
        other.alive_.reset();
    }
    return *this;
}




