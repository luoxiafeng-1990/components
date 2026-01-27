#pragma once

/**
 * @file BufferConsumerStrategies.hpp
 * @brief 消费者策略实现库（第三部分：策略实现部分）
 * 
 * 此文件包含所有消费者策略的具体实现类。
 * 这些类都继承自 IBufferConsumer 接口，提供不同的消费算法。
 * 
 * 设计模式：策略模式（Strategy Pattern）
 * - IBufferConsumer：策略接口（定义在 BufferConsumer.hpp）
 * - 本文件中的类：具体策略实现
 * 
 * 策略列表：
 * - DisplayConsumer：显示策略
 * - FileWriterConsumer：单文件写入策略
 * - MultiChannelFileWriterConsumer：多通道文件写入策略
 * - EncodedStreamWriterConsumer：编码流写入策略
 * - CompareConsumer：Buffer比较策略（已废弃）
 */

#include "productionline/consumer/BufferConsumer.hpp"  // 包含 IBufferConsumer 接口
#include <string>
#include <vector>
#include <memory>
#include <log4cplus/logger.h>

// 前向声明
class LinuxFramebufferDevice;

// FFmpeg 前向声明
extern "C" {
struct AVCodecParameters;
struct AVRational;
}

namespace productionline {
namespace consumer {

/**
 * @brief 显示消费者（DisplayConsumer）
 * 
 * 将 Buffer 显示到 Linux Framebuffer 设备
 */
class DisplayConsumer : public IBufferConsumer {
public:
    /**
     * @brief 构造函数
     * @param display 显示设备指针
     * @param ch0_enable 是否启用通道0（默认 true）
     * @param ch1_enable 是否启用通道1（默认 true）
     */
    explicit DisplayConsumer(LinuxFramebufferDevice* display,
                             bool ch0_enable = true,
                             bool ch1_enable = true);
    
    bool initialize(Buffer* first_buffer) override;
    bool consume(Buffer* buffer, int channel_id) override;
    std::string getStats() const override;
    bool shouldConsumeChannel(int channel_id) const override;

private:
    LinuxFramebufferDevice* display_;
    bool ch0_enable_;
    bool ch1_enable_;
    int success_count_;
    int failed_count_;
    int total_count_;
    log4cplus::Logger logger_;
};

/**
 * @brief 文件写入消费者（单文件）
 * 
 * 支持通道过滤：可以配置只处理 ch0 或 ch1
 */
class FileWriterConsumer : public IBufferConsumer {
public:
    /**
     * @brief 构造函数
     * @param output_path 输出文件路径
     * @param enable_ch0 是否启用通道0（默认 true）
     * @param enable_ch1 是否启用通道1（默认 false）
     */
    explicit FileWriterConsumer(const std::string& output_path,
                                bool enable_ch0 = true,
                                bool enable_ch1 = false);
    ~FileWriterConsumer();
    
    bool initialize(Buffer* first_buffer) override;
    bool consume(Buffer* buffer, int channel_id) override;
    void cleanup() override;
    std::string getStats() const override;
    bool shouldConsumeChannel(int channel_id) const override;

private:
    std::unique_ptr<io::BufferWriter> writer_;
    std::string output_path_;
    bool initialized_;
    bool enable_ch0_;
    bool enable_ch1_;
    int write_count_;
    int failed_count_;
    log4cplus::Logger logger_;
};

/**
 * @brief 多通道文件写入消费者（支持 PP0/PP1 双通道）
 */
class MultiChannelFileWriterConsumer : public IBufferConsumer {
public:
    MultiChannelFileWriterConsumer(
        const std::vector<std::string>& output_paths,
        bool enable_ch0, bool enable_ch1);
    ~MultiChannelFileWriterConsumer();
    
    bool initialize(Buffer* first_buffer) override;
    bool consume(Buffer* buffer, int channel_id) override;
    void cleanup() override;
    std::string getStats() const override;
    bool shouldConsumeChannel(int channel_id) const override;

private:
    std::vector<std::unique_ptr<io::BufferWriter>> writers_;
    std::vector<std::string> output_paths_;
    std::vector<bool> initialized_;
    bool enable_ch0_;
    bool enable_ch1_;
    int write_count_;
    int failed_count_;
    log4cplus::Logger logger_;
};

/**
 * @brief 编码流写入消费者（MP4 封装）
 * 
 * 用于录制编码流（不解码，直接 remux）
 */
class EncodedStreamWriterConsumer : public IBufferConsumer {
public:
    EncodedStreamWriterConsumer(
        const std::string& output_path,
        const AVCodecParameters* codec_params,
        AVRational time_base);
    ~EncodedStreamWriterConsumer();
    
    bool initialize(Buffer* first_buffer) override;
    bool consume(Buffer* buffer, int channel_id) override;
    void cleanup() override;
    std::string getStats() const override;

private:
    std::unique_ptr<io::BufferWriter> writer_;
    std::string output_path_;
    const AVCodecParameters* codec_params_;
    AVRational time_base_;
    bool initialized_;
    int packet_count_;
    int64_t total_bytes_;
    int failed_count_;
    log4cplus::Logger logger_;
};

/**
 * @brief 比较消费者（BufferComparator）
 * 
 * 用于对比两个解码器的输出（硬件 vs 软件）
 * 
 * ⚠️ 已废弃：此消费者不支持PTS对齐，仅用于简单的顺序对比。
 * 请使用 DualBufferCompareService 进行带PTS对齐的对比。
 * 
 * 此类的实现已注释，如需使用请取消注释。
 */
/*
class CompareConsumer : public IBufferConsumer {
public:
    CompareConsumer(
        io::BufferComparator* comparator,
        std::shared_ptr<BufferPool> reference_pool);
    
    bool initialize(Buffer* first_buffer) override;
    bool consume(Buffer* buffer, int channel_id) override;
    std::string getStats() const override;

private:
    io::BufferComparator* comparator_;
    std::shared_ptr<BufferPool> reference_pool_;
    int compare_count_;
    int success_count_;
    int failed_count_;
    log4cplus::Logger logger_;
};
*/

} // namespace consumer
} // namespace productionline
