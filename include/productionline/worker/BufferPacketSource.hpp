#ifndef BUFFER_PACKET_SOURCE_HPP
#define BUFFER_PACKET_SOURCE_HPP

#include "productionline/worker/IPacketSource.hpp"
#include "buffer/bufferpool/Buffer.hpp"
#include <memory>
#include <string>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <map>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

// 前向声明
struct AVCodecParameters;
struct AVPacket;
class BufferPool;
class BufferPacketSource;  // BufferPacketSource 前向声明

// ⭐ v2.22 修改：移除 PacketGuard RAII 包装器
// 原因：新的三状态 API（acquire/commit/cancel）提供了更精确的控制
// Worker 需要根据解码结果决定是 commit 还是 cancel，RAII 模式不再适用

/**
 * @brief BufferPacketSource - Buffer 数据源实现
 * 
 * 功能：直接从 BufferPool 获取 filled Buffer（已由 Record Worker 填充）
 * 
 * 使用场景：
 * - MultiWorkerProductionLine 场景
 * - 从 Record Worker 的 BufferPool 直接获取 packet
 * 
 * 工作流程（v2.13 重构后）：
 * 1. Record Worker 读取 RTSP 流，填充 AVPacket 到 BufferPool
 * 2. BufferPacketSource 关联 Record Worker 的 BufferPool
 * 3. readPacket() 时：acquireFilled() → 复制 AVPacket → releaseFilled()
 * 4. 传递给解码器进行解码
 * 
 * 优势：
 * - 数据源自己负责从哪里获取数据（符合抽象语义）
 * - 无需 MultiWorkerPL 做中间复制
 * - 代码更简洁，职责更清晰
 * 
 * ⭐ v2.18 新增：共享模式（发布-订阅）
 * 
 * 功能：在 ONE_TO_MANY 模式下，确保所有消费者处理同一个 packet
 * 
 * 工作流程：
 * 1. MultiWorkerProductionLine 创建唯一的 BufferPacketSource 实例（共享模式）
 * 2. 所有消费者 Worker 持有同一个实例（shared_ptr）
 * 3. 每个 Worker 调用 readPacket() 时：
 *    - 增加请求计数器
 *    - 等待所有订阅者都请求
 *    - Publisher 获取新 Buffer，所有订阅者读取同一个 packet
 *    - 所有订阅者完成后，Publisher 释放 Buffer
 * 4. 确保所有消费者处理的是同一个 packet（真正的共享）
 */
class BufferPacketSource : public IPacketSource {
public:
    /**
     * @brief 构造函数（普通模式）
     * @param codec_params 编解码器参数（从 Record Worker 获取）
     */
    explicit BufferPacketSource(const AVCodecParameters* codec_params);
    
    /**
     * @brief 构造函数（共享模式 - 用于 MultiWorker ONE_TO_MANY）
     * @param codec_params 编解码器参数（从 Record Worker 获取）
     * @param subscriber_count 订阅者数量（消费者 Worker 数量）
     * 
     * ⭐ v2.18 新增：共享模式构造函数
     * 
     * 说明：
     * - MultiWorkerProductionLine 创建唯一实例时使用此构造函数
     * - subscriber_count 必须 > 1（否则使用普通模式）
     * - 此实例会被所有消费者 Worker 共享（通过 shared_ptr）
     */
    explicit BufferPacketSource(const AVCodecParameters* codec_params, size_t subscriber_count);
    
    /**
     * @brief 析构函数
     */
    ~BufferPacketSource() override;
    
    // 禁止拷贝
    BufferPacketSource(const BufferPacketSource&) = delete;
    BufferPacketSource& operator=(const BufferPacketSource&) = delete;
    
    // IPacketSource 接口实现
    bool open() override;
    void close() override;
    bool isOpen() const override;
    int readPacket(AVPacket* packet) override;
    const AVCodecParameters* getCodecParameters() const override;
    int getVideoStreamIndex() const override;
    int getTotalFrames() const override;
    long getFileSize() const override;
    std::string getFilePath() const override;
    
    /**
     * @brief 定位到指定帧索引（Buffer 模式不支持）
     * @param frame_index 帧索引
     * @return 总是返回 false（Buffer 模式不支持 seek）
     */
    bool seek(int frame_index) override;
    bool isAtEnd() const override;
    int getSourceWidth() const override;
    int getSourceHeight() const override;
    AVPixelFormat getSourcePixelFormat() const override;
    SourceType getDataSourceType() const override;
    /**
     * @brief 设置数据源 BufferPool（v2.13 新增）
     * @param pool_weak Record Worker 的 BufferPool（weak_ptr）
     * 
     * 说明：BufferPacketSource 会直接从这个 BufferPool 的 filled queue 获取数据
     */
    void setSourceBufferPool(std::weak_ptr<BufferPool> pool_weak);
    
    /**
     * @brief 获取 AVPacket 指针（共享模式，v3.0 新架构）
     * @param worker_id Worker 的唯一标识（通常是 this 指针）
     * @return AVPacket* 指针（零拷贝），nullptr=EOF 或已获取过当前版本
     * 
     * 说明：
     * - 只在共享模式下使用
     * - 阻塞等待直到有新 buffer 或 EOF
     * - 防止同一个 Worker 重复获取同一个 buffer（通过版本号机制）
     * - 不递减 remaining_subscribers_（由 commitPacket 负责）
     * 
     * 返回值：
     * - 非空：成功获取 packet 指针
     * - nullptr：EOF 或已获取过当前版本（需要等待新 buffer）
     * 
     * 使用方式：
     * ```cpp
     * AVPacket* packet = ps->acquirePacket(this);
     * if (!packet) {
     *     return false;  // EOF 或等待新 buffer
     * }
     * // 使用 packet...
     * ```
     */
    AVPacket* acquirePacket(void* worker_id);
    
    /**
     * @brief 提交释放 AVPacket（共享模式，v3.0 新架构）
     * @param worker_id Worker 的唯一标识
     * @return true=成功提交, false=失败（状态不对）
     * 
     * 说明：
     * - 只有成功处理（解码出至少一帧）后才调用
     * - 递减 remaining_subscribers_
     * - 如果是最后一个订阅者，唤醒 Fetch 任务
     * - 重置 Worker 状态，允许获取下一个 buffer
     * 
     * 使用方式：
     * ```cpp
     * if (decoded_at_least_one_frame) {
     *     ps->commitPacket(this);
     * }
     * ```
     */
    bool commitPacket(void* worker_id);
    
    /**
     * @brief 取消当前获取（共享模式，v3.0 新架构）
     * @param worker_id Worker 的唯一标识
     * 
     * 说明：
     * - 失败时调用（如 send_packet 失败、receive_frame 失败）
     * - 不递减 remaining_subscribers_（保持订阅者计数不变）
     * - 重置 Worker 状态，允许重新获取当前 buffer（重试）
     * 
     * 使用方式：
     * ```cpp
     * if (send_packet_failed) {
     *     ps->cancelPacket(this);
     *     return false;  // 重试
     * }
     * ```
     */
    void cancelPacket(void* worker_id);
    
private:
    // ========== 通用成员（普通模式和共享模式都使用）==========
    const AVCodecParameters* codec_params_;     // 编解码器参数（从 Record Worker 获取）
    std::weak_ptr<BufferPool> source_pool_;     // ⭐ v2.13：关联的 BufferPool（从 Record Worker）
    std::atomic<bool> is_open_;                 // 🎯 原子变量，保证线程安全的状态检查
    
    // ========== 共享模式成员（v2.18 新增，v3.0 扩展）==========
    bool is_shared_mode_;                       // 是否为共享模式
    size_t total_subscribers_;                  // 订阅者总数（消费者数量）
    std::atomic<size_t> remaining_subscribers_; // 剩余未完成的订阅者数量
    Buffer* current_buffer_;                    // 当前共享的 Buffer
    mutable std::mutex mutex_;                  // 互斥锁（保护共享状态）
    std::condition_variable cv_subscribers_;    // 条件变量（订阅者等待新 Buffer）
    std::condition_variable cv_fetch_;          // 条件变量（Fetch 任务等待订阅者完成）
    std::condition_variable cv_task_exit_;      // 条件变量（等待 Fetch 任务退出）
    std::atomic<bool> is_running_;              // 是否运行中
    std::atomic<bool> fetch_task_running_;      // Fetch 任务是否正在运行
    
    // ========== v3.0 新增：版本号和 Worker 状态追踪 ==========
    std::atomic<uint64_t> current_buffer_version_{0};  // 当前 buffer 的版本号（递增）
    
    /**
     * @brief Worker 状态
     * 
     * 用于追踪每个 Worker 对当前 buffer 的状态
     */
    struct WorkerState {
        uint64_t acquired_version = 0;    // Worker 获取的 buffer 版本号
        bool has_acquired = false;        // 是否已获取当前版本
        bool has_committed = false;       // 是否已 commit 当前版本（防止重复 commit）
    };
    
    std::map<void*, WorkerState> worker_states_;  // Worker ID (this指针) -> 状态
    
    /**
     * @brief 从当前 Buffer 复制 packet 数据
     * @param dst_packet 目标 packet
     * @param src_packet 源 packet（从 Buffer 获取）
     * @return 0=成功, <0=错误
     */
    int copyPacket(AVPacket* dst_packet, const AVPacket* src_packet);
    
    /**
     * @brief Fetch 任务函数（在全局线程池中运行）
     */
    void fetchTaskFunc();
    
    // 日志器
    log4cplus::Logger logger_;
};

#endif // BUFFER_PACKET_SOURCE_HPP

