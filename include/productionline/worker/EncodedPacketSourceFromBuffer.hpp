#ifndef ENCODED_PACKET_SOURCE_FROM_BUFFER_HPP
#define ENCODED_PACKET_SOURCE_FROM_BUFFER_HPP

#include "productionline/worker/IEncodedPacketSource.hpp"
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
class EncodedPacketSourceFromBuffer;  // 前向声明

// ⭐ v2.22 修改：移除 PacketGuard RAII 包装器
// 原因：新的三状态 API（acquire/commit/cancel）提供了更精确的控制
// Worker 需要根据解码结果决定是 commit 还是 cancel，RAII 模式不再适用

// ============================================================
// PacketAcquireResult - Packet 获取结果（v2.31 新增）
// ============================================================

/**
 * @brief Packet 获取结果状态
 * 
 * v2.31 新增：用于 acquireEncodedPacket 的返回值
 */
enum class AcquireStatus : int {
    Success = 0,           ///< 成功获取
    Eof = -1,              ///< 数据流结束（正常）
    Again = -2,            ///< 已处理当前版本，需等待新数据
    InvalidMode = -3,      ///< 非共享模式
    NoData = -4,           ///< 无可用数据
    Stopped = -5           ///< 已停止
};

/**
 * @brief 获取状态的字符串描述
 */
inline const char* acquireStatusToString(AcquireStatus status) {
    switch (status) {
        case AcquireStatus::Success:     return "Success";
        case AcquireStatus::Eof:         return "EOF";
        case AcquireStatus::Again:       return "Again";
        case AcquireStatus::InvalidMode: return "InvalidMode";
        case AcquireStatus::NoData:      return "NoData";
        case AcquireStatus::Stopped:     return "Stopped";
        default:                         return "Unknown";
    }
}

/**
 * @brief Packet 获取结果（借用语义）
 * 
 * v2.31 新增：封装状态和借用的 AVPacket 指针
 * 
 * 设计原则（参考 Google StatusOr + Rust Result）：
 * - 明确的成功/失败状态
 * - 只有成功时才能访问 packet
 * - 零拷贝，零额外分配
 * - 借用语义：不负责 packet 生命周期
 * 
 * 使用示例：
 * @code
 * auto result = ps->acquireEncodedPacket(this);
 * 
 * if (result.ok()) {
 *     AVPacket* pkt = result.packet();
 *     // 或使用箭头运算符
 *     int64_t pts = result->pts;
 * } else if (result.isEof()) {
 *     // 正常结束
 * } else if (result.shouldRetry()) {
 *     // 等待新数据
 * }
 * @endcode
 */
class PacketAcquireResult {
public:
    // ===== 工厂方法 =====
    
    /// 成功结果
    static PacketAcquireResult success(AVPacket* packet) {
        return PacketAcquireResult(AcquireStatus::Success, packet);
    }
    
    /// EOF 结果
    static PacketAcquireResult eof() {
        return PacketAcquireResult(AcquireStatus::Eof, nullptr);
    }
    
    /// 需要重试
    static PacketAcquireResult again() {
        return PacketAcquireResult(AcquireStatus::Again, nullptr);
    }
    
    /// 失败结果
    static PacketAcquireResult failure(AcquireStatus status) {
        return PacketAcquireResult(status, nullptr);
    }
    
    // ===== 状态查询 =====
    
    /// 获取状态
    AcquireStatus status() const noexcept { return status_; }
    
    /// 是否成功
    bool ok() const noexcept { return status_ == AcquireStatus::Success; }
    
    /// 是否到达 EOF（正常结束）
    bool isEof() const noexcept { return status_ == AcquireStatus::Eof; }
    
    /// 是否需要重试（等待新数据）
    bool shouldRetry() const noexcept { return status_ == AcquireStatus::Again; }
    
    /// 是否是错误（非 Success 且非 EOF 且非 Again）
    bool isError() const noexcept {
        return status_ != AcquireStatus::Success && 
               status_ != AcquireStatus::Eof &&
               status_ != AcquireStatus::Again;
    }
    
    /// 隐式 bool 转换（方便条件判断）
    explicit operator bool() const noexcept { return ok(); }
    
    // ===== 数据访问 =====
    
    /**
     * @brief 获取 AVPacket 指针
     * 
     * @warning 仅当 ok() 为 true 时有效！
     * @note 返回的是借用指针，不要 av_packet_free
     */
    AVPacket* packet() const noexcept { return packet_; }
    
    /**
     * @brief 箭头运算符（便捷访问 AVPacket 成员）
     * 
     * @code
     * if (result.ok()) {
     *     int64_t pts = result->pts;
     *     int size = result->size;
     * }
     * @endcode
     */
    AVPacket* operator->() const noexcept { return packet_; }
    
    /**
     * @brief 获取状态描述
     */
    const char* statusString() const noexcept { return acquireStatusToString(status_); }

private:
    PacketAcquireResult(AcquireStatus status, AVPacket* packet)
        : status_(status), packet_(packet) {}
    
    AcquireStatus status_;
    AVPacket* packet_;  // 借用指针，不负责生命周期
};

/**
 * @brief EncodedPacketSourceFromBuffer - 从 Buffer 读取编码数据的数据源实现
 * 
 * 功能：直接从 BufferPool 获取 filled Buffer（已由 Record Worker 填充）
 * 
 * 使用场景：
 * - MultiWorkerProductionLine 场景
 * - 从 Record Worker 的 BufferPool 直接获取编码后的 packet
 * 
 * 工作流程（v2.13 重构后）：
 * 1. Record Worker 读取 RTSP 流，填充 AVPacket 到 BufferPool
 * 2. EncodedPacketSourceFromBuffer 关联 Record Worker 的 BufferPool
 * 3. readEncodedPacket() 时：acquireFilled() → 复制 AVPacket → releaseFilled()
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
 * 1. MultiWorkerProductionLine 创建唯一的 EncodedPacketSourceFromBuffer 实例（共享模式）
 * 2. 所有消费者 Worker 持有同一个实例（shared_ptr）
 * 3. 每个 Worker 调用 readEncodedPacket() 时：
 *    - 增加请求计数器
 *    - 等待所有订阅者都请求
 *    - Publisher 获取新 Buffer，所有订阅者读取同一个 packet
 *    - 所有订阅者完成后，Publisher 释放 Buffer
 * 4. 确保所有消费者处理的是同一个 packet（真正的共享）
 */
class EncodedPacketSourceFromBuffer : public IEncodedPacketSource {
public:
    /**
     * @brief 构造函数（普通模式）
     * @param codec_params 编解码器参数（从 Record Worker 获取）
     */
    explicit EncodedPacketSourceFromBuffer(const AVCodecParameters* codec_params);
    
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
    explicit EncodedPacketSourceFromBuffer(const AVCodecParameters* codec_params, size_t subscriber_count);
    
    /**
     * @brief 析构函数
     */
    ~EncodedPacketSourceFromBuffer() override;
    
    // 禁止拷贝
    EncodedPacketSourceFromBuffer(const EncodedPacketSourceFromBuffer&) = delete;
    EncodedPacketSourceFromBuffer& operator=(const EncodedPacketSourceFromBuffer&) = delete;
    
    // ============ IDataSourceNavigator 接口实现 ============
    
    // 数据源生命周期
    bool open() override;
    bool open(const char* path) override;     // 返回 false（Buffer 模式不支持路径）
    void close() override;
    bool isOpen() const override;
    
    // 数据源导航（Buffer 模式不支持导航）
    /**
     * @brief 定位到指定帧索引（Buffer 模式不支持）
     * @param frame_index 帧索引
     * @return 总是返回 false（Buffer 模式不支持 seek）
     */
    bool seek(int frame_index) override;
    bool seekToBegin() override;              // 返回 false（Buffer 模式不支持）
    bool seekToEnd() override;                // 返回 false（Buffer 模式不支持）
    bool skip(int frame_count) override;      // 返回 false（Buffer 模式不支持）
    
    // 数据源状态查询
    int getTotalFrames() const override;      // 返回 -1（流式数据无总帧数）
    int getCurrentFrameIndex() const override;// 返回已读取的帧数
    size_t getFrameSize() const override;     // 返回 0（无法估算）
    long getFileSize() const override;        // 返回 -1（无文件大小）
    std::string getPath() const override;     // 返回 "BufferPool"
    bool hasMoreFrames() const override;      // 返回 !isAtEnd()
    bool isAtEnd() const override;
    
    // 数据源属性
    int getSourceWidth() const override;
    int getSourceHeight() const override;
    AVPixelFormat getSourcePixelFormat() const override;
    const AVCodecParameters* getCodecParameters() const override;
    SourceType getDataSourceType() const override;
    
    // ============ IEncodedPacketSource 特有方法 ============
    int readEncodedPacket(AVPacket* packet) override;
    int getVideoStreamIndex() const override;

    /**
     * @brief 设置数据源 BufferPool（v2.13 新增）
     * @param pool_weak Record Worker 的 BufferPool（weak_ptr）
     * 
     * 说明：EncodedPacketSourceFromBuffer 会直接从这个 BufferPool 的 filled queue 获取数据
     */
    void setSourceBufferPool(std::weak_ptr<BufferPool> pool_weak);
    
    /**
     * @brief 获取编码后的 AVPacket（共享模式，v3.0 新架构）
     * @param worker_id Worker 的唯一标识（通常是 this 指针）
     * @return PacketAcquireResult 结果对象
     * 
     * 说明：
     * - 只在共享模式下使用
     * - 阻塞等待直到有新 buffer 或 EOF
     * - 防止同一个 Worker 重复获取同一个 buffer（通过版本号机制）
     * - 不递减 remaining_subscribers_（由 commitEncodedPacket 负责）
     * 
     * v2.31 修改：返回类型从 AVPacket* 改为 PacketAcquireResult
     * 
     * 返回值状态：
     * - Success：成功获取，可通过 packet() 获取指针
     * - Eof：数据流正常结束
     * - Again：已处理当前版本，需等待新 buffer
     * - InvalidMode：非共享模式
     * - NoData：无可用数据（异常）
     * 
     * 使用方式：
     * ```cpp
     * auto result = ps->acquireEncodedPacket(this);
     * if (result.ok()) {
     *     AVPacket* pkt = result.packet();
     *     // 或使用箭头运算符: result->pts
     * } else if (result.isEof()) {
     *     // 正常结束
     * } else if (result.shouldRetry()) {
     *     // 等待新数据
     * }
     * ```
     */
    PacketAcquireResult acquireEncodedPacket(void* worker_id);
    
    /**
     * @brief 提交释放编码后的 AVPacket（共享模式，v3.0 新架构）
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
     *     ps->commitEncodedPacket(this);
     * }
     * ```
     */
    bool commitEncodedPacket(void* worker_id);
    
    /**
     * @brief 取消当前获取（共享模式，v2.22 新架构）
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
     *     ps->cancelEncodedPacket(this);
     *     return false;  // 重试
     * }
     * ```
     */
    void cancelEncodedPacket(void* worker_id);
    
    /**
     * @brief 获取当前 buffer 版本号（v2.23 新增）
     * @return 当前 buffer 版本号
     * 
     * 说明：
     * - 用于 WorkerSyncCoordinator 关联同一帧的多个 Worker
     * - 版本号在 fetchTaskFunc 中递增
     */
    uint64_t getCurrentBufferVersion() const {
        return current_buffer_version_.load(std::memory_order_acquire);
    }
    
   
private:
    // ========== 通用成员（普通模式和共享模式都使用）==========
    const AVCodecParameters* codec_params_;     // 编解码器参数（从 Record Worker 获取）
    std::weak_ptr<BufferPool> source_pool_;     // ⭐ v2.13：关联的 BufferPool（从 Record Worker）
    std::atomic<bool> is_open_;                 // 原子变量，保证线程安全的状态检查
    std::atomic<int> current_frame_index_;      // 当前帧索引（已读取的帧数）
    
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

#endif // ENCODED_PACKET_SOURCE_FROM_BUFFER_HPP
