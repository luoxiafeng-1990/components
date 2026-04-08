/**
 * @file BufferConsumerStrategies.hpp
 * @brief Buffer 消费策略实现声明
 * 
 * 提供多种消费策略：
 * - CountConsumer: 仅统计帧数
 * - DisplayConsumer: 显示输出（Framebuffer）
 * - SaveRawConsumer: 保存原始 YUV/RGB
 * - SaveEncodedConsumer: 保存编码流
 * - JpegEncodeConsumer: JPEG 编码预览（v3.3）
 * - MultiConsumer: 多策略组合
 * 
 * 注：PSNR/SSIM 比较功能已迁移至 WorkerSyncCoordinator::createDefaultCompareCallback
 */

#ifndef BUFFER_CONSUMER_STRATEGIES_HPP
#define BUFFER_CONSUMER_STRATEGIES_HPP

#include "consumptionline/IBufferConsumer.hpp"
#include "consumptionline/BufferWriter.hpp"
#include "consumptionline/BufferComparator.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "vendor/contracts/IDisplayDevice.hpp"
#include "vendor/taco/display/DisplayDeviceFactory.hpp"
#include "productionline/worker/FFmpegEncodeWorker.hpp"
#include "productionline/worker/RawFrameSourceFromBuffer.hpp"

#include <log4cplus/logger.h>
#include <memory>
#include <atomic>
#include <string>
#include <vector>
#include <thread>
#include <map>

extern "C" {
#include <libavutil/frame.h>
}

#include "opencv2/core.hpp"
#include "opencv2/core/tacv.hpp"
#include "opencv2/imgproc.hpp"
#include "opencv2/imgcodecs.hpp"

class VideoProductionLine;

namespace consumer {

using consumptionline::io::BufferComparator;

// ============================================================
// CountConsumer - 仅统计帧数
// ============================================================

/**
 * @brief 仅统计帧数的消费者
 * 
 * 最简单的消费者，只统计处理的帧数，不做任何实际处理。
 * 适用于性能测试等场景。
 */
class CountConsumer : public IBufferConsumer {
public:
    CountConsumer() = default;
    ~CountConsumer() override = default;
    
    bool consume(const std::vector<Buffer*>& buffers, int frame_index) override;
    std::string getStats() const override;
    
    /**
     * @brief 获取已消费帧数
     */
    int getFrameCount() const { return frame_count_.load(); }
    
private:
    std::atomic<int> frame_count_{0};
};

// ============================================================
// DisplayConsumer - 显示输出
// ============================================================

/**
 * @brief 显示输出消费者
 * 
 * 将 Buffer 数据输出到 Linux Framebuffer 设备显示。
 */
class DisplayConsumer : public IBufferConsumer {
public:
    using DisplayConsumerConfig = WorkerConfig::ConsumerTypeConfig::DisplayConsumerConfig;

    /**
     * @brief 构造函数
     * @param config 显示消费配置（通过 vendor 决定使用哪种显示设备）
     */
    explicit DisplayConsumer(const DisplayConsumerConfig& config);
    ~DisplayConsumer() override;
    
    bool initialize(const std::vector<Buffer*>& first_buffers) override;
    bool consume(const std::vector<Buffer*>& buffers, int frame_index) override;
    void finalize() override;
    std::string getStats() const override;
    bool shouldRetainBuffer() const override;
    
private:
    DisplayConsumerConfig config_;
    std::unique_ptr<IDisplayDevice> display_;
    int success_count_ = 0;
    int failed_count_ = 0;
    bool initialized_ = false;
    bool last_consume_failed_ = false;
};

// ============================================================
// SaveRawConsumer - 保存原始 YUV/RGB
// ============================================================

/**
 * @brief 保存原始数据消费者（支持多通道）
 * 
 * 将 Buffer 中的原始 YUV/RGB 数据保存到文件。
 * 支持多通道输出：根据 Buffer::getOutputChannel() 自动分发到对应的 Writer。
 * 支持每个通道独立的帧数限制。
 */
class SaveRawConsumer : public IBufferConsumer {
public:
    /**
     * @brief 构造函数（单路径，兼容旧代码）
     * @param output_path 输出文件路径
     * @param max_frames 最大保存帧数（-1=全部）
     */
    SaveRawConsumer(const std::string& output_path, int max_frames = -1);
    
    /**
     * @brief 构造函数（多路径，支持多通道）
     * @param output_paths 输出文件路径列表（按通道顺序）
     * @param max_frames_per_channel 每个通道的最大保存帧数（-1=全部）
     */
    SaveRawConsumer(const std::vector<std::string>& output_paths, 
                    const std::vector<int>& max_frames_per_channel);
    
    ~SaveRawConsumer() override;
    
    bool initialize(const std::vector<Buffer*>& first_buffers) override;
    bool consume(const std::vector<Buffer*>& buffers, int frame_index) override;
    void finalize() override;
    std::string getStats() const override;
    
    /**
     * @brief 获取已保存帧数（所有通道总计）
     */
    int getSavedCount() const;
    
    /**
     * @brief 获取指定通道的已保存帧数
     */
    int getSavedCount(int channel) const;
    
private:
    std::vector<std::string> output_paths_;           ///< 输出路径列表
    std::vector<int> max_frames_per_channel_;         ///< 每个通道的最大保存帧数
    std::map<int, std::unique_ptr<consumptionline::io::BufferWriter>> writers_;  ///< 通道 → Writer
    std::map<int, int> saved_counts_;                  ///< 通道 → 已保存帧数
    bool initialized_ = false;
    
    /// 获取或创建指定通道的 Writer（延迟初始化）
    consumptionline::io::BufferWriter* getOrCreateWriter(int channel, Buffer* sample_buffer);
    
    /// 获取指定通道的输出路径
    std::string getOutputPath(int channel) const;
    
    /// 获取指定通道的最大帧数
    int getMaxFrames(int channel) const;
};

// ============================================================
// SaveEncodedConsumer - 保存编码流
// ============================================================

/**
 * @brief 保存编码流消费者
 * 
 * 将 Buffer 中的编码数据（如 H.264/H.265 码流）保存到文件。
 */
class SaveEncodedConsumer : public IBufferConsumer {
public:
    /**
     * @brief 构造函数
     * @param output_path 输出文件路径
     * @param codec_params 编解码器参数
     * @param time_base 时间基准
     */
    SaveEncodedConsumer(
        const std::string& output_path,
        const struct AVCodecParameters* codec_params,
        AVRational time_base
    );
    ~SaveEncodedConsumer() override;
    
    bool initialize(const std::vector<Buffer*>& first_buffers) override;
    bool consume(const std::vector<Buffer*>& buffers, int frame_index) override;
    void finalize() override;
    std::string getStats() const override;
    
private:
    std::string output_path_;
    const struct AVCodecParameters* codec_params_;
    AVRational time_base_;
    std::unique_ptr<consumptionline::io::BufferWriter> writer_;
    int packet_count_ = 0;
    bool initialized_ = false;
};

// ============================================================
// ChannelCompareConsumer - 通道比较（v2.27）
// ============================================================

/**
 * @brief 通道比较消费者（v2.27 新增）
 * 
 * 主动从 BufferPool 获取 buffer，比较指定两个通道的输出。
 * 假设多通道输出是交替的（同一 PTS 的两个通道连续输出）。
 * 
 * 使用场景：PP 模块多通道输出比较（如 pp0 vs pp1）
 * 
 * 注意：此消费者使用主动获取模式，需要调用 run() 方法启动消费循环，
 * 而不是被动等待 consume() 调用。
 */
class ChannelCompareConsumer : public IBufferConsumer {
public:
    /**
     * @brief 构造函数
     * @param pool BufferPool 指针（主动获取模式）
     * @param config 比较配置
     */
    ChannelCompareConsumer(
        std::shared_ptr<BufferPool> pool,
        const WorkerConfig::ConsumerTypeConfig::CompareType& config
    );
    ~ChannelCompareConsumer() override;
    
    bool initialize(const std::vector<Buffer*>& first_buffers) override;
    
    /// 被动消费接口（不使用，保留兼容）
    bool consume(const std::vector<Buffer*>& buffers, int frame_index) override;
    
    /**
     * @brief 主动消费循环（核心方法）
     * @param max_frames 最大比较帧数（-1 = 无限制）
     */
    void run(int max_frames = -1);
    
    /**
     * @brief 停止消费循环
     */
    void stop();
    
    void finalize() override;
    std::string getStats() const override;
    
    /// 获取平均 PSNR
    double getAveragePsnr() const;
    
    /// 获取平均 SSIM
    double getAverageSsim() const;
    
    /// 是否通过验证
    bool isPassed() const { return passed_; }
    
    /// 获取已比较帧数
    int getComparedCount() const { return compared_count_; }
    
    /// 获取 PTS 不匹配次数
    int getMismatchCount() const { return mismatch_count_; }
    
private:
    std::shared_ptr<BufferPool> pool_;
    WorkerConfig::ConsumerTypeConfig::CompareType config_;
    std::unique_ptr<consumptionline::io::BufferComparator> comparator_;
    
    std::atomic<bool> running_{false};
    
    int compared_count_ = 0;
    int mismatch_count_ = 0;
    double psnr_sum_ = 0.0;
    double ssim_sum_ = 0.0;
    bool passed_ = true;
    bool initialized_ = false;
};

// ============================================================
// OpencvConsumer - Buffer→Mat转换并计算PSNR/SSIM
// ============================================================

/**
 * @brief OpenCV 消费者
 *
 * 将 Buffer 转换为 cv::Mat（BGR 格式），然后：
 * - 单 Buffer：仅转换并统计
 * - 多 Buffer（≥2）：调用 BufferComparator 计算 PSNR/SSIM
 *
 * 使用场景：
 * - SINGLE 模式：验证解码结果可正常转为 Mat
 * - COMPARE 模式（通过 callback_chain）：比较两路解码器输出的质量差异
 */
class OpencvConsumer : public IBufferConsumer {
public:
    using OpencvType = WorkerConfig::ConsumerTypeConfig::OpencvType;
    using CompareType = WorkerConfig::ConsumerTypeConfig::CompareType;

    OpencvConsumer(const OpencvType& opencv_config, const CompareType& compare_config);
    ~OpencvConsumer() override;

    bool initialize(const std::vector<Buffer*>& first_buffers) override;
    bool consume(const std::vector<Buffer*>& buffers, int frame_index) override;
    void finalize() override;
    std::string getStats() const override;

    // ---- 结果查询 ----
    int  getFrameCount()    const { return frames_processed_; }
    int  getCompareCount()  const { return frames_compared_; }
    double getAveragePsnr() const;
    double getAverageSsim() const;
    bool isPassed()         const;

private:
    cv::Mat bufferToMat(Buffer* buf) const;
    cv::Mat applyOpencvTransform(const cv::Mat& src, int frame_index = -1) const;

    OpencvType opencv_config_;
    CompareType compare_config_;
    std::unique_ptr<consumptionline::io::BufferComparator> comparator_;

    int    frames_processed_ = 0;
    int    frames_compared_  = 0;
    double psnr_sum_         = 0.0;
    double ssim_sum_         = 0.0;
    bool   passed_           = true;
    bool   initialized_      = false;
};

// ============================================================
// JpegEncodeConsumer - JPEG 编码预览（v3.3）
// ============================================================

/**
 * @brief JPEG 编码消费者
 *
 * 将解码帧编码为 JPEG 输出到命名管道（供 WebUI 预览）。
 * 内部复用 FFmpegEncodeWorker（jpeg_taco / mjpeg），通过
 * RawFrameSourceFromBuffer 直接模式桥接 consume() 与 fillBuffer()。
 */
class JpegEncodeConsumer : public IBufferConsumer {
public:
    using Config = WorkerConfig::ConsumerTypeConfig::JpegEncodeType;

    explicit JpegEncodeConsumer(const Config& config);
    ~JpegEncodeConsumer() override;

    bool initialize(const std::vector<Buffer*>& first_buffers) override;
    bool consume(const std::vector<Buffer*>& buffers, int frame_index) override;
    void finalize() override;
    std::string getStats() const override;

private:
    log4cplus::Logger logger_;
    Config config_;
    Config::FrameCallback on_frame_;

    std::shared_ptr<RawFrameSourceFromBuffer> raw_source_;
    std::unique_ptr<::VideoProductionLine> encode_pipeline_;
    uint64_t encode_pool_id_ = 0;

    std::thread reader_thread_;
    std::atomic<bool> reader_running_{false};
    void readerThreadFunc();

    int pipe_fd_ = -1;
    int frame_interval_ = 1;

    std::atomic<int> encoded_count_{0};
    std::atomic<int> skipped_count_{0};
    std::atomic<int> error_count_{0};

    bool openPipe();
    void writeToPipe(const uint8_t* data, int size);
};

// ============================================================
// MultiConsumer - 多策略组合
// ============================================================

/**
 * @brief 多策略组合消费者
 * 
 * 用于消费类型叠加场景（如 CONSUME_DISPLAY | CONSUME_SAVE_RAW）。
 * 内部持有多个策略，依次调用每个策略的 consume 方法。
 */
class MultiConsumer : public IBufferConsumer {
public:
    MultiConsumer() = default;
    ~MultiConsumer() override = default;
    
    /**
     * @brief 添加子策略
     * @param strategy 子策略
     */
    void addStrategy(std::shared_ptr<IBufferConsumer> strategy);
    
    bool initialize(const std::vector<Buffer*>& first_buffers) override;
    bool consume(const std::vector<Buffer*>& buffers, int frame_index) override;
    void finalize() override;
    std::string getStats() const override;
    
    /**
     * @brief 获取子策略数量
     */
    size_t getStrategyCount() const { return strategies_.size(); }
    
private:
    std::vector<std::shared_ptr<IBufferConsumer>> strategies_;
};

} // namespace consumer

#endif // BUFFER_CONSUMER_STRATEGIES_HPP
