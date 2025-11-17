#pragma once

#include "Buffer.hpp"
#include "BufferHandle.hpp"
#include "BufferAllocator.hpp"
#include <string>
#include <vector>
#include <queue>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <memory>

/**
 * @brief BufferPool - 核心 Buffer 调度器
 * 
 * 职责：
 * - 管理 buffer 的分配和生命周期
 * - 提供生产者/消费者队列（free_queue, filled_queue）
 * - 支持自有内存和外部内存托管
 * - 提供线程安全的 acquire/release 接口
 * 
 * 特性：
 * - 三种构造方式（自有/外部简单/外部生命周期检测）
 * - 物理地址感知（虚拟+物理）
 * - 外部 buffer 生命周期检测（weak_ptr 语义）
 * - DMA-BUF 导出支持
 */
class BufferPool {
public:
    // ========== 外部 Buffer 信息结构 ==========
    struct ExternalBufferInfo {
        void* virt_addr;        // 虚拟地址
        uint64_t phys_addr;     // 物理地址（0表示未知，需自动获取）
        size_t size;            // Buffer 大小
    };
    
    // ========== 构造方式 1: 自己分配 buffer ==========
    /**
     * @brief 创建 BufferPool（自有内存）
     * @param count Buffer 数量
     * @param size 每个 Buffer 的大小
     * @param use_cma 是否使用 CMA/DMA 连续物理内存
     * @param name Pool 名称（用于全局注册和调试）
     * @param category Pool 分类（如 "Display", "Video", "Network"）
     * @throws std::runtime_error 如果分配失败
     */
    BufferPool(int count, size_t size, bool use_cma = false,
               const std::string name = "UnnamedPool",
               const std::string category = "");
    
    // ========== 构造方式 2: 托管外部 buffer（简单版）==========
    /**
     * @brief 创建 BufferPool（托管外部buffer）
     * @param external_buffers 外部 buffer 信息数组
     * @param name Pool 名称（用于全局注册和调试）
     * @param category Pool 分类（如 "Display", "Video", "Network"）
     * @note BufferPool 只管理调度，不负责释放这些 buffer
     */
    BufferPool(const std::vector<ExternalBufferInfo>& external_buffers,
               const std::string name = "UnnamedPool",
               const std::string category = "");
    
    // ========== 构造方式 3: 托管外部 buffer（带生命周期检测）==========
    /**
     * @brief 创建 BufferPool（托管外部buffer + 生命周期检测）
     * @param handles BufferHandle 数组（转移所有权）
     * @param name Pool 名称（用于全局注册和调试）
     * @param category Pool 分类（如 "Display", "Video", "Network"）
     * @note BufferPool 会保存 weak_ptr 用于检测 buffer 是否失效
     */
    BufferPool(std::vector<std::unique_ptr<BufferHandle>> handles,
               const std::string name = "UnnamedPool",
               const std::string category = "");
    
    // ========== 构造方式 4: 动态注入模式（初始为空）==========
    /**
     * @brief 创建 BufferPool（动态注入模式 - 初始为空）
     * 
     * 适用场景：
     * - RTSP 流解码（AVFrame 动态注入）
     * - 网络接收器（零拷贝动态注入）
     * - 任何需要运行时动态填充 buffer 的场景
     * 
     * 工作流程：
     * 1. 创建空的 BufferPool（没有预分配 buffer）
     * 2. 生产者通过 injectFilledBuffer() 动态注入 buffer
     * 3. 消费者正常使用 acquireFilled() / releaseFilled()
     * 
     * 特点：
     * - Pool 纯粹作为队列调度器，不拥有任何预分配的 buffer
     * - 所有 buffer 都通过 injectFilledBuffer() 运行时注入
     * - 适合 RTSP 等动态解码场景，对用户透明
     * 
     * @param name Pool 名称（用于全局注册和调试）
     * @param category Pool 分类（如 "Display", "Video", "RTSP"）
     * @param max_capacity 最大容量限制（0 表示无限制）
     * 
     * @note 这个模式下 getTotalCount() 会返回当前已注入的 buffer 数量（动态变化）
     * 
     * 使用示例：
     * @code
     * // 创建动态注入模式的 BufferPool（初始为空）
     * BufferPool rtsp_pool("RTSP_Decoder_Pool", "RTSP", 10);  // 最多缓存10帧
     * 
     * // RtspVideoReader 内部会动态注入解码后的 AVFrame
     * VideoProducer producer(rtsp_pool);
     * producer.start(config);
     * 
     * // 消费循环（对用户透明）
     * while (running) {
     *     Buffer* buf = rtsp_pool.acquireFilled(true, 100);
     *     display.displayBufferByDMA(buf);
     *     rtsp_pool.releaseFilled(buf);
     * }
     * @endcode
     */
    BufferPool(const std::string name, const std::string category, size_t max_capacity = 0);
    
    /**
     * @brief 析构函数 - 释放资源
     */
    ~BufferPool();
    
    // 禁止拷贝和赋值
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;
    
    // ========== 生产者接口 ==========
    
    /**
     * @brief 获取空闲 buffer（生产者使用）
     * @param blocking 是否阻塞等待
     * @param timeout_ms 超时时间（毫秒），-1 表示无限等待
     * @return Buffer* 成功返回 buffer，失败返回 nullptr
     */
    Buffer* acquireFree(bool blocking = true, int timeout_ms = -1);
    
    /**
     * @brief 提交填充好的 buffer（生产者使用）
     * @param buffer 填充好的 buffer
     */
    void submitFilled(Buffer* buffer);
    
    // ========== 消费者接口 ==========
    
    /**
     * @brief 获取就绪 buffer（消费者使用）
     * @param blocking 是否阻塞等待
     * @param timeout_ms 超时时间（毫秒），-1 表示无限等待
     * @return Buffer* 成功返回 buffer，失败返回 nullptr
     */
    Buffer* acquireFilled(bool blocking = true, int timeout_ms = -1);
    
    /**
     * @brief 归还已使用的 buffer（消费者使用）
     * @param buffer 已使用的 buffer
     */
    void releaseFilled(Buffer* buffer);
    
    // ========== 动态注入接口（用于零拷贝场景）==========
    
    /**
     * @brief 动态注入外部已填充的 buffer
     * 
     * 适用场景：
     * - 硬件解码器输出buffer
     * - 网络接收的零拷贝buffer
     * - RTSP流解码的AVFrame
     * 
     * 工作流程：
     * 1. 将外部buffer包装为临时Buffer对象
     * 2. 直接放入filled_queue供消费者使用
     * 3. 消费者调用releaseFilled时，触发deleter回收
     * 
     * @param handle 外部buffer（转移所有权，包含deleter）
     * @return Buffer* 注入后的Buffer指针，失败返回nullptr
     * 
     * @note 
     * - 注入的buffer标记为 Ownership::EXTERNAL
     * - deleter 中应该回收buffer供生产者重用
     * - 如果队列满（达到限制），可能拒绝注入
     */
    Buffer* injectFilledBuffer(std::unique_ptr<BufferHandle> handle);
    
    /**
     * @brief 弹出并销毁临时buffer（内部清理机制）
     * 
     * 用于清理已失效的外部buffer
     * 
     * @param buffer 要移除的buffer
     * @return true 如果成功移除
     */
    bool ejectBuffer(Buffer* buffer);
    
    // ========== 查询接口 ==========
    
    /// 获取空闲 buffer 数量
    int getFreeCount() const;
    
    /// 获取就绪 buffer 数量
    int getFilledCount() const;
    
    /// 获取总 buffer 数量
    int getTotalCount() const;
    
    /// 获取单个 buffer 大小
    size_t getBufferSize() const;
    
    /**
     * @brief 设置 buffer 大小（仅用于动态注入模式）
     * 
     * 适用场景：
     * - 动态注入模式下，初始 buffer_size_ == 0
     * - 第一次解码/接收到帧后，需要设置 buffer 大小
     * - 设置后锁定，不允许修改，确保所有注入的 buffer 大小一致
     * 
     * @param size Buffer 大小（字节）
     * @return true 设置成功，false 设置失败（已经设置过或参数无效）
     * 
     * @note 线程安全：内部使用 mutex 保护
     * @note 仅在 buffer_size_ == 0 时可以设置（动态注入模式）
     * @note 设置后不可修改，确保所有注入的 buffer 大小一致
     * 
     * 使用示例：
     * @code
     * BufferPool pool("RTSP_Pool", "RTSP", 10);  // 动态注入模式
     * // ... 解码第一帧，得到 frame_size
     * if (pool.getBufferSize() == 0) {
     *     pool.setBufferSize(frame_size);  // 设置 buffer 大小
     * }
     * @endcode
     */
    bool setBufferSize(size_t size);
    
    /**
     * @brief 通过 ID 查找 buffer
     * @param id Buffer ID
     * @return Buffer* 成功返回 buffer，失败返回 nullptr
     */
    Buffer* getBufferById(uint32_t id);
    const Buffer* getBufferById(uint32_t id) const;
    
    // ========== Registry 相关接口 ==========
    
    /**
     * @brief 获取 Pool 名称
     * @return const std::string& Pool 名称
     */
    const std::string& getName() const { return name_; }
    
    /**
     * @brief 获取 Pool 分类
     * @return const std::string& Pool 分类
     */
    const std::string& getCategory() const { return category_; }
    
    /**
     * @brief 获取注册表 ID
     * @return uint64_t 在 BufferPoolRegistry 中的唯一 ID
     */
    uint64_t getRegistryId() const { return registry_id_; }
    
    // ========== 校验接口 ==========
    
    /**
     * @brief 校验单个 buffer 是否有效
     * @param buffer 待校验的 buffer
     * @return true 如果 buffer 有效（基础校验 + 所有权检查 + 生命周期检测）
     */
    bool validateBuffer(const Buffer* buffer) const;
    
    /**
     * @brief 校验所有 buffer
     * @return true 如果所有 buffer 都有效
     */
    bool validateAllBuffers() const;
    
    // ========== 调试接口 ==========
    
    /// 打印统计信息
    void printStats() const;
    
    /// 打印所有 buffer 详情
    void printAllBuffers() const;
    
    // ========== 高级功能：导出 DMA-BUF fd ==========
    
    /**
     * @brief 导出 buffer 为 DMA-BUF fd（用于跨进程共享）
     * @param buffer_id Buffer ID
     * @return DMA-BUF fd，失败返回 -1
     * @note 仅 CMA/DMA buffer 支持导出
     */
    int exportBufferAsDmaBuf(uint32_t buffer_id);
    
private:
    // ========== 内部初始化方法 ==========
    
    void initializeOwnedBuffers(int count, size_t size, bool use_cma);
    void initializeExternalBuffers(const std::vector<ExternalBufferInfo>& infos);
    void initializeFromHandles(std::vector<std::unique_ptr<BufferHandle>> handles);
    
    // ========== 辅助方法 ==========
    
    /// 检查 buffer 是否属于本 pool
    bool verifyBufferOwnership(const Buffer* buffer) const;
    
    /// 获取物理地址（通过 allocator）
    uint64_t getPhysicalAddress(void* virt_addr);
    
    // ========== 成员变量 ==========
    
    // Registry 相关
    std::string name_;                    // Pool 名称
    std::string category_;                // Pool 分类
    uint64_t registry_id_;                // 在 BufferPoolRegistry 中的唯一 ID
    
    // Buffer 池
    size_t buffer_size_;                              // 单个 buffer 大小
    size_t max_capacity_;                             // 最大容量限制（0 表示无限制，用于动态注入模式）
    std::vector<Buffer> buffers_;                     // Buffer 对象池
    std::unordered_map<uint32_t, Buffer*> buffer_map_; // ID -> Buffer* 快速索引
    
    // 内存分配器（策略模式）
    std::unique_ptr<BufferAllocator> allocator_;
    
    // 外部 buffer 生命周期跟踪
    std::vector<std::unique_ptr<BufferHandle>> external_handles_;  // 持有所有权
    std::vector<std::weak_ptr<bool>> lifetime_trackers_;           // 生命周期检测
    
    // 队列（生产者-消费者模型）
    std::queue<Buffer*> free_queue_;      // 空闲队列
    std::queue<Buffer*> filled_queue_;    // 就绪队列
    
    // 动态注入的临时buffer管理
    std::vector<std::unique_ptr<Buffer>> transient_buffers_;       // 临时buffer对象
    std::unordered_map<Buffer*, std::unique_ptr<BufferHandle>> transient_handles_;  // Buffer* -> Handle映射
    std::mutex transient_mutex_;          // 保护临时buffer列表
    
    // 同步原语
    mutable std::mutex mutex_;            // 保护队列和状态
    std::condition_variable free_cv_;     // 空闲队列条件变量
    std::condition_variable filled_cv_;   // 就绪队列条件变量
    
    // 统计
    uint32_t next_buffer_id_;             // 下一个分配的 Buffer ID
};

