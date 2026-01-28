/**
 * @file BufferConsumerStrategies.hpp
 * @brief Buffer 消费策略实现声明
 * 
 * 提供多种消费策略：
 * - CountConsumer: 仅统计帧数
 * - DisplayConsumer: 显示输出（Framebuffer）
 * - SaveRawConsumer: 保存原始 YUV/RGB
 * - SaveEncodedConsumer: 保存编码流
 * - CompareConsumer: PSNR/SSIM 对比
 * - MultiConsumer: 多策略组合
 */

#ifndef BUFFER_CONSUMER_STRATEGIES_HPP
#define BUFFER_CONSUMER_STRATEGIES_HPP

#include "productionline/io/IBufferConsumer.hpp"
#include "productionline/io/BufferWriter.hpp"
#include "productionline/io/BufferComparator.hpp"
#include "display/LinuxFramebufferDevice.hpp"

#include <memory>
#include <atomic>
#include <string>
#include <vector>

namespace consumer {

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
    /**
     * @brief 构造函数
     * @param device_id Framebuffer 设备 ID（默认 0）
     */
    explicit DisplayConsumer(int device_id = 0);
    ~DisplayConsumer() override;
    
    bool initialize(const std::vector<Buffer*>& first_buffers) override;
    bool consume(const std::vector<Buffer*>& buffers, int frame_index) override;
    void finalize() override;
    std::string getStats() const override;
    
private:
    int device_id_;
    std::unique_ptr<LinuxFramebufferDevice> display_;
    int success_count_ = 0;
    int failed_count_ = 0;
    bool initialized_ = false;
};

// ============================================================
// SaveRawConsumer - 保存原始 YUV/RGB
// ============================================================

/**
 * @brief 保存原始数据消费者
 * 
 * 将 Buffer 中的原始 YUV/RGB 数据保存到文件。
 */
class SaveRawConsumer : public IBufferConsumer {
public:
    /**
     * @brief 构造函数
     * @param output_path 输出文件路径
     * @param max_frames 最大保存帧数（-1=全部）
     */
    SaveRawConsumer(const std::string& output_path, int max_frames = -1);
    ~SaveRawConsumer() override;
    
    bool initialize(const std::vector<Buffer*>& first_buffers) override;
    bool consume(const std::vector<Buffer*>& buffers, int frame_index) override;
    void finalize() override;
    std::string getStats() const override;
    
    /**
     * @brief 获取已保存帧数
     */
    int getSavedCount() const { return saved_count_; }
    
private:
    std::string output_path_;
    int max_frames_;
    std::unique_ptr<productionline::io::BufferWriter> writer_;
    int saved_count_ = 0;
    bool initialized_ = false;
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
    std::unique_ptr<productionline::io::BufferWriter> writer_;
    int packet_count_ = 0;
    bool initialized_ = false;
};

// ============================================================
// CompareConsumer - PSNR/SSIM 对比
// ============================================================

/**
 * @brief 对比消费者（PSNR/SSIM）
 * 
 * 对比两个 Buffer 的图像质量，计算 PSNR 和 SSIM。
 * 用于 COMPARE 模式，验证硬件解码器输出质量。
 */
class CompareConsumer : public IBufferConsumer {
public:
    /**
     * @brief 构造函数
     * @param min_psnr 最小 PSNR 阈值（低于此值视为失败）
     * @param min_ssim 最小 SSIM 阈值（低于此值视为失败）
     * @param enable_psnr 是否启用 PSNR 计算
     * @param enable_ssim 是否启用 SSIM 计算
     */
    CompareConsumer(
        double min_psnr = 30.0,
        double min_ssim = 0.95,
        bool enable_psnr = true,
        bool enable_ssim = true
    );
    ~CompareConsumer() override;
    
    bool initialize(const std::vector<Buffer*>& first_buffers) override;
    bool consume(const std::vector<Buffer*>& buffers, int frame_index) override;
    void finalize() override;
    std::string getStats() const override;
    
    /**
     * @brief 获取平均 PSNR
     */
    double getAveragePsnr() const;
    
    /**
     * @brief 获取平均 SSIM
     */
    double getAverageSsim() const;
    
    /**
     * @brief 是否通过验证
     */
    bool isPassed() const;
    
    /**
     * @brief 获取对比帧数
     */
    int getComparedCount() const { return compared_count_; }
    
private:
    double min_psnr_;
    double min_ssim_;
    bool enable_psnr_;
    bool enable_ssim_;
    std::unique_ptr<productionline::io::BufferComparator> comparator_;
    int compared_count_ = 0;
    double psnr_sum_ = 0.0;
    double ssim_sum_ = 0.0;
    bool passed_ = true;
    bool initialized_ = false;
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
