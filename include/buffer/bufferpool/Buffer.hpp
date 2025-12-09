#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <functional>

/**
 * @brief Buffer 元数据类
 * 
 * 封装单个 buffer 的完整元数据，包括：
 * - 唯一ID（用于硬件回调识别）
 * - 虚拟地址（CPU访问）
 * - 物理地址（DMA/硬件访问）
 * - 所有权类型（自有/外部）
 * - 状态机（FREE/ACQUIRED/FILLED/IN_USE）
 * - 引用计数（生命周期跟踪）
 */
class Buffer {
public:
    // 所有权类型
    enum class Ownership {
        OWNED,      // BufferPool 拥有并管理生命周期
        EXTERNAL    // 外部拥有，BufferPool 只负责调度
    };
    
    // Buffer 状态（用于调试和校验）
    enum class State {
        IDLE,                    // 空闲，等待生产者获取（在 free_queue）
        LOCKED_BY_PRODUCER,      // 被生产者锁定，正在填充数据
        READY_FOR_CONSUME,       // 数据就绪，等待消费者获取（在 filled_queue）
        LOCKED_BY_CONSUMER       // 被消费者锁定，正在使用数据
    };
    
    /**
     * @brief 构造函数
     * @param id 唯一标识符
     * @param virt_addr 虚拟地址（用户空间）
     * @param phys_addr 物理地址（硬件/DMA，0表示未知）
     * @param size Buffer 大小（字节）
     * @param ownership 所有权类型
     */
    Buffer(uint32_t id, 
           void* virt_addr, 
           uint64_t phys_addr,
           size_t size,
           Ownership ownership);
    
    ~Buffer() = default;
    
    // 禁止拷贝（Buffer 不应该被拷贝）
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    
    // 允许移动（用于 vector 的 resize/reserve）
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;
    
    // ========== Getters ==========
    
    /// 获取唯一ID
    uint32_t id() const { return id_; }
    
    /// 获取虚拟地址（CPU访问）
    void* getVirtualAddress() const { return virt_addr_; }
    
    /// 获取物理地址（DMA/硬件访问）
    uint64_t getPhysicalAddress() const { return phys_addr_; }
    
    /// 获取Buffer大小
    size_t size() const { return size_; }
    
    /// 获取所有权类型
    Ownership ownership() const { return ownership_; }
    
    /// 获取当前状态
    State state() const { return state_.load(); }
    
    /// 获取 DMA-BUF fd（如果有）
    int getDmaBufFd() const { return dma_fd_; }
    
    /// 别名（兼容旧代码）
    void* data() const { return virt_addr_; }
    
    // ========== Setters ==========
    
    /// 设置状态
    void setState(State state) { state_.store(state); }
    
    /// 设置物理地址（用于延迟获取物理地址的场景，如零拷贝解码）
    void setPhysicalAddress(uint64_t phys_addr) { phys_addr_ = phys_addr; }
    
    /// 设置 DMA-BUF fd（用于共享/导出）
    void setDmaBufFd(int fd) { dma_fd_ = fd; }
    
    // ========== 引用计数（用于外部buffer生命周期检测）==========
    
    /// 增加引用计数
    void addRef() { ref_count_.fetch_add(1); }
    
    /// 减少引用计数
    void releaseRef() { 
        int old_count = ref_count_.fetch_sub(1);
        if (old_count <= 0) {
            // 警告：引用计数异常
            // 不在这里处理，由 BufferPool 检测
        }
    }
    
    /// 获取当前引用计数
    int refCount() const { return ref_count_.load(); }
    
    // ========== 校验接口 ==========
    
    /**
     * @brief 基础校验：检查Buffer是否仍然有效
     * @return true 如果 magic number 正确且地址非空
     */
    bool isValid() const { 
        return validation_magic_ == MAGIC_NUMBER && virt_addr_ != nullptr;
    }
    
    /**
     * @brief 完整校验：包含用户自定义校验回调
     * @return true 如果基础校验通过且自定义校验通过（如果有）
     */
    bool validate() const;
    
    /**
     * @brief 自定义校验回调类型
     */
    using ValidationCallback = std::function<bool(const Buffer*)>;
    
    /**
     * @brief 设置自定义校验回调（高级功能）
     * @param cb 校验函数，返回 true 表示有效
     */
    void setValidationCallback(ValidationCallback cb) {
        validation_callback_ = cb;
    }
    
    // ========== 调试接口 ==========
    
    /**
     * @brief 打印Buffer详细信息
     */
    void printInfo() const;
    
    /**
     * @brief 获取状态字符串
     */
    static const char* stateToString(State state);
    
    /**
     * @brief 获取所有权类型字符串
     */
    static const char* ownershipToString(Ownership ownership);
    
private:
    // ========== 核心属性 ==========
    uint32_t id_;                    // 唯一标识
    void* virt_addr_;                // 虚拟地址（用户空间）
    uint64_t phys_addr_;             // 物理地址（硬件/DMA）
    size_t size_;                    // Buffer 大小
    Ownership ownership_;            // 所有权类型
    
    // ========== 状态管理 ==========
    std::atomic<State> state_;       // 当前状态（线程安全）
    std::atomic<int> ref_count_;     // 引用计数（外部buffer检测）
    
    // ========== DMA/共享相关 ==========
    int dma_fd_;                     // DMA-BUF file descriptor
    
    // ========== 安全性 ==========
    static constexpr uint32_t MAGIC_NUMBER = 0xBEEFF123;  // 魔数：BEEF + F123
    uint32_t validation_magic_;      // 魔数，用于检测野指针
    
    // ========== 高级校验 ==========
    ValidationCallback validation_callback_;
    
    // ========== 扩展元数据（可根据需求添加）==========
    // uint64_t timestamp_;          // 填充时间戳
    // int frame_index_;             // 帧索引
    // void* user_data_;             // 用户自定义数据
};
