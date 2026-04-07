#ifndef RAW_FRAME_SOURCE_FROM_BUFFER_HPP
#define RAW_FRAME_SOURCE_FROM_BUFFER_HPP

#include "productionline/worker/IRawFrameSource.hpp"
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <map>

extern "C" {
#include <libavutil/pixfmt.h>
}

// 前向声明
class BufferPool;
class Buffer;

/**
 * @brief RawFrameSourceFromBuffer - 从 BufferPool 读取原始帧数据
 * 
 * 功能：从 BufferPool 获取已解码的 AVFrame（用于 Pipeline：解码→编码）
 * 
 * 使用场景：
 * - 解码 → 编码 Pipeline（从解码 Worker 的 BufferPool 获取帧）
 * - MultiWorkerProductionLine 中的编码 Worker
 * - 实时转码场景
 * 
 * 设计与 EncodedPacketSourceFromBuffer 对称：
 * - EncodedPacketSourceFromBuffer：从 BufferPool 获取 AVPacket（供解码 Worker）
 * - RawFrameSourceFromBuffer：从 BufferPool 获取 AVFrame（供编码 Worker）
 * 
 * 命名规范：
 * - 遵循 EncodedPacketSourceFromBuffer 的命名模式
 * - RawFrame = 原始帧（已解码的 YUV/RGB 数据）
 * - FromBuffer = 数据来源是 BufferPool
 * 
 * 工作流程：
 * 1. 编码 Worker 创建 RawFrameSourceFromBuffer 实例
 * 2. MultiWorkerProductionLine 调用 setSourceBufferPool() 关联解码 Worker 的 BufferPool
 * 3. readRawFrame() 时：acquireFilled() → 复制 AVFrame → releaseFilled()
 * 4. 传递给编码器进行编码
 * 
 * ⭐ 共享模式（发布-订阅）：
 * 功能：在 ONE_TO_MANY 模式下，确保所有消费者处理同一个 frame
 * - MultiWorkerProductionLine 创建唯一的 RawFrameSourceFromBuffer 实例（共享模式）
 * - 所有消费者 Worker 持有同一个实例（shared_ptr）
 * - 每个 Worker 调用 acquireRawFrame() 获取帧指针，commitRawFrame() 完成处理
 * 
 * ⭐ 直接模式（v3.3 新增）：
 * 功能：不走 BufferPool，由调用者通过 setFrame() 直接注入 AVFrame* 指针
 * - JpegEncodeConsumer 在 consume() 中先 setFrame()，再触发 fillBuffer()
 * - readRawFrame() 立即返回注入的帧，无阻塞
 * - 用于在 IBufferConsumer 消费循环中同步复用 FFmpegEncodeWorker
 */
class RawFrameSourceFromBuffer : public IRawFrameSource {
public:
    /**
     * @brief 构造函数（普通模式）
     * @param width 期望的帧宽度（0=从源 Buffer 获取）
     * @param height 期望的帧高度（0=从源 Buffer 获取）
     * @param pix_fmt 期望的像素格式（AV_PIX_FMT_NONE=从源 Buffer 获取）
     */
    RawFrameSourceFromBuffer(int width = 0,
                              int height = 0,
                              AVPixelFormat pix_fmt = AV_PIX_FMT_NONE);
    
    /**
     * @brief 构造函数（共享模式 - 用于 MultiWorker ONE_TO_MANY）
     * @param width 期望的帧宽度
     * @param height 期望的帧高度
     * @param pix_fmt 期望的像素格式
     * @param subscriber_count 订阅者数量（编码 Worker 数量）
     * 
     * 说明：
     * - MultiWorkerProductionLine 创建唯一实例时使用此构造函数
     * - subscriber_count 必须 > 1（否则使用普通模式）
     * - 此实例会被所有消费者 Worker 共享（通过 shared_ptr）
     */
    RawFrameSourceFromBuffer(int width,
                              int height,
                              AVPixelFormat pix_fmt,
                              size_t subscriber_count);
    
    /**
     * @brief 构造函数（直接模式 - 用于 JpegEncodeConsumer 同步编码）
     * @param width 帧宽度
     * @param height 帧高度
     * @param pix_fmt 像素格式
     * @param direct_mode 必须为 true（区分与共享模式构造函数的重载）
     *
     * 说明：
     * - 不使用 BufferPool，由调用者通过 setFrame() 注入帧
     * - readRawFrame() 直接返回注入的帧，无阻塞
     */
    RawFrameSourceFromBuffer(int width,
                              int height,
                              AVPixelFormat pix_fmt,
                              bool direct_mode);

    /**
     * @brief 析构函数
     */
    ~RawFrameSourceFromBuffer() override;
    
    // 禁止拷贝
    RawFrameSourceFromBuffer(const RawFrameSourceFromBuffer&) = delete;
    RawFrameSourceFromBuffer& operator=(const RawFrameSourceFromBuffer&) = delete;
    
    /**
     * @brief 设置源 BufferPool（从解码 Worker 获取）
     * @param pool_weak 解码 Worker 的 BufferPool（weak_ptr）
     * 
     * 说明：RawFrameSourceFromBuffer 会从这个 BufferPool 的 filled queue 获取数据
     */
    void setSourceBufferPool(std::weak_ptr<BufferPool> pool_weak);
    
    /**
     * @brief 直接注入帧指针（直接模式专用）
     * @param frame 要编码的 AVFrame 指针（不接管所有权）
     *
     * 在 consume() 中调用后，紧接着调用 FFmpegEncodeWorker::fillBuffer()，
     * 内部的 readRawFrame() 会立即取走此帧。
     */
    void setFrame(AVFrame* frame);
    
    // ============ IRawFrameSource 接口实现 ============
    int readRawFrame(AVFrame* frame) override;
    int getFrameWidth() const override { return width_; }
    int getFrameHeight() const override { return height_; }
    int getFramePixelFormat() const override { return static_cast<int>(pix_fmt_); }
    
    // ============ IDataSourceNavigator 接口实现 ============
    
    // 数据源生命周期
    bool open() override;
    bool open(const char* path) override;
    void close() override;
    bool isOpen() const override;
    
    // 数据源导航（Buffer 模式不支持导航）
    bool seek(int frame_index) override;
    bool seekToBegin() override;
    bool seekToEnd() override;
    bool skip(int frame_count) override;
    
    // 数据源状态查询
    int getTotalFrames() const override;
    int getCurrentFrameIndex() const override;
    size_t getFrameSize() const override;
    long getFileSize() const override;
    std::string getPath() const override;
    bool hasMoreFrames() const override;
    bool isAtEnd() const override;
    
    // 数据源属性
    int getSourceWidth() const override { return width_; }
    int getSourceHeight() const override { return height_; }
    AVPixelFormat getSourcePixelFormat() const override { return pix_fmt_; }
    const AVCodecParameters* getCodecParameters() const override { return nullptr; }
    SourceType getDataSourceType() const override { return SourceType::BUFFER_SOURCE; }
    
    // ============ 共享模式 API（与 EncodedPacketSourceFromBuffer 对称）============
    
    /**
     * @brief 获取原始帧指针（共享模式，零拷贝）
     * @param worker_id Worker 的唯一标识（通常是 this 指针）
     * @return AVFrame* 指针，nullptr=EOF 或已获取过当前版本
     * 
     * 说明：
     * - 只在共享模式下使用
     * - 阻塞等待直到有新 buffer 或 EOF
     * - 防止同一个 Worker 重复获取同一个 buffer（通过版本号机制）
     * - 不递减 remaining_subscribers_（由 commitRawFrame 负责）
     */
    AVFrame* acquireRawFrame(void* worker_id);
    
    /**
     * @brief 提交释放原始帧（共享模式）
     * @param worker_id Worker 的唯一标识
     * @return true=成功提交, false=失败（状态不对）
     * 
     * 说明：
     * - 只有成功处理（编码出至少一个 packet）后才调用
     * - 递减 remaining_subscribers_
     * - 如果是最后一个订阅者，唤醒 Fetch 任务
     * - 重置 Worker 状态，允许获取下一个 buffer
     */
    bool commitRawFrame(void* worker_id);
    
    /**
     * @brief 取消当前获取（共享模式）
     * @param worker_id Worker 的唯一标识
     * 
     * 说明：
     * - 失败时调用（如 send_frame 失败）
     * - 不递减 remaining_subscribers_（保持订阅者计数不变）
     * - 重置 Worker 状态，允许重新获取当前 buffer（重试）
     */
    void cancelRawFrame(void* worker_id);
    
    /**
     * @brief 获取当前 buffer 版本号
     * @return 当前 buffer 版本号
     * 
     * 说明：用于同步多个 Worker 处理同一帧
     */
    uint64_t getCurrentBufferVersion() const {
        return current_buffer_version_.load(std::memory_order_acquire);
    }

private:
    // ========== 通用成员 ==========
    std::weak_ptr<BufferPool> source_pool_weak_;  // 源 BufferPool
    int width_;                                    // 帧宽度
    int height_;                                   // 帧高度
    AVPixelFormat pix_fmt_;                        // 像素格式
    std::atomic<bool> is_open_;                    // 打开状态
    std::atomic<int> current_frame_index_;         // 当前帧索引
    
    // ========== 模式标志 ==========
    bool is_direct_mode_;                          // 是否为直接模式（v3.3）
    AVFrame* direct_frame_;                        // 直接模式：注入的帧指针

    // ========== 共享模式成员 ==========
    bool is_shared_mode_;                          // 是否为共享模式
    size_t total_subscribers_;                     // 订阅者总数
    std::atomic<size_t> remaining_subscribers_;    // 剩余未完成的订阅者
    Buffer* current_buffer_;                       // 当前共享的 Buffer
    mutable std::mutex mutex_;                     // 互斥锁
    std::condition_variable cv_subscribers_;       // 订阅者等待条件变量
    std::condition_variable cv_fetch_;             // Fetch 任务等待条件变量
    std::condition_variable cv_task_exit_;         // 任务退出条件变量
    std::atomic<bool> is_running_;                 // 运行状态
    std::atomic<bool> fetch_task_running_;         // Fetch 任务运行状态
    std::atomic<uint64_t> current_buffer_version_{0};  // 当前 buffer 版本号
    
    // Worker 状态追踪
    struct WorkerState {
        uint64_t acquired_version = 0;    // Worker 获取的 buffer 版本号
        bool has_acquired = false;        // 是否已获取当前版本
        bool has_committed = false;       // 是否已 commit 当前版本
    };
    std::map<void*, WorkerState> worker_states_;
    
    /**
     * @brief 复制 AVFrame 数据
     * @param dst_frame 目标帧
     * @param src_frame 源帧
     * @return 0=成功, <0=错误
     */
    int copyFrame(AVFrame* dst_frame, const AVFrame* src_frame);
    
    /**
     * @brief Fetch 任务函数（共享模式，在全局线程池中运行）
     */
    void fetchTaskFunc();
    
    // 日志器
    log4cplus::Logger logger_;
};

#endif // RAW_FRAME_SOURCE_FROM_BUFFER_HPP
