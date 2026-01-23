#pragma once

#include "buffer/bufferpool/Buffer.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "productionline/VideoProductionLine.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include <functional>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <log4cplus/logger.h>

// FFmpeg 前向声明
extern "C" {
struct AVCodecParameters;
struct AVRational;
struct AVFrame;
#define AV_NOPTS_VALUE ((int64_t)UINT64_C(0x8000000000000000))
}

// 前向声明
namespace productionline {
namespace io {
    class BufferWriter;
    class BufferComparator;
}
namespace consumer {
    class DualBufferCompareService;  // 前向声明
}
}

class LinuxFramebufferDevice;

namespace productionline {
namespace consumer {

/**
 * @brief Buffer 消费者接口（策略模式）
 * 
 * 职责：
 * - 定义如何消费单个 Buffer
 * - 支持不同消费策略（显示、写入、比较等）
 * 
 * 设计原则：
 * - 单一职责：只负责消费逻辑
 * - 无状态：消费操作应该是无状态的（或通过构造函数注入状态）
 */
class IBufferConsumer {
public:
    virtual ~IBufferConsumer() = default;
    
    /**
     * @brief 消费一个 Buffer
     * @param buffer 待消费的 Buffer
     * @param channel_id 输出通道 ID（0 或 1，用于多通道场景）
     * @return true 成功，false 失败
     */
    virtual bool consume(Buffer* buffer, int channel_id) = 0;
    
    /**
     * @brief 初始化消费者（可选，在开始消费前调用）
     * @param first_buffer 第一个 Buffer（用于检测格式、初始化资源等）
     * @return true 成功，false 失败
     */
    virtual bool initialize(Buffer* first_buffer) { 
        (void)first_buffer;
        return true; 
    }
    
    /**
     * @brief 清理资源（可选，在消费结束后调用）
     */
    virtual void cleanup() {}
    
    /**
     * @brief 获取消费统计信息（用于报告）
     * @return 统计信息字符串
     */
    virtual std::string getStats() const { return ""; }
    
    /**
     * @brief 检查是否应该消费指定通道的 Buffer
     * @param channel_id 通道 ID
     * @return true 应该消费，false 跳过
     * 
     * 默认实现：消费所有通道
     */
    virtual bool shouldConsumeChannel(int channel_id) const { 
        (void)channel_id;
        return true; 
    }
};

/**
 * @brief 一键运行选项（封装 open → run → printStats → close 流程）
 */
struct RunOptions {
    /**
     * @brief 运行标志指针（可选）
     * 
     * - 如果为 nullptr，内部会创建一个本地 running_flag，从 true 开始运行，
     *   直到达到 max_frames / producer 停止 / 超时 等条件。
     * - 如果由调用方传入，则由调用方控制何时置 false 以提前停止。
     */
    std::atomic<bool>* running_flag = nullptr;
    
    /**
     * @brief 运行结束后是否自动打印统计信息
     */
    bool auto_print_stats = true;
    
    /**
     * @brief 运行结束后是否自动关闭服务
     */
    bool auto_close = true;
    
    /**
     * @brief 错误回调（可选）
     * 
     * @note 等价于直接传给 open() 的 error_callback。
     */
    std::function<void(const std::string&)> error_callback = nullptr;
};

/**
 * @brief BufferConsumerService - 消费者服务类（门面模式）
 * 
 * 职责：
 * - 封装完整的消费流程（创建生产线 → 启动 → 消费循环 → 清理）
 * - 管理生产线的生命周期
 * - 支持多种消费策略（通过 IBufferConsumer 接口）
 * 
 * 设计模式：
 * - 门面模式：简化复杂的消费流程
 * - 策略模式：支持不同的消费策略
 * - 模板方法模式：定义固定的消费流程骨架
 * 
 * 使用方式（类似 BufferWriter/BufferComparator）：
 * ```cpp
 * BufferConsumerService service;
 * DisplayConsumer consumer(&display);
 * 
 * BufferConsumerService::Config config;
 * config.worker_config = WorkerConfigBuilder()...build();
 * 
 * if (service.open(config, &consumer)) {
 *     service.run(running_flag);
 *     service.close();
 * }
 * ```
 */
class BufferConsumerService {
public:
    /**
     * @brief 服务配置
     */
    struct Config {
        WorkerConfig worker_config;           // Worker 配置（必需）
        bool loop = false;                     // 是否循环播放
        int thread_count = 1;                  // 线程数
        bool enable_monitor = false;           // 是否启用监控
        int max_frames = -1;                   // 最大帧数（-1表示无限制，双通道时建议翻倍）
        int acquire_timeout_ms = 100;          // 获取 Buffer 超时时间（毫秒）
        int max_timeout_count = 50;            // 最大超时次数（超过后停止）
        bool drain_remaining = true;           // 是否排空剩余 Buffer
        bool wait_first_buffer = true;         // 是否等待第一个 Buffer（用于初始化消费者）
        int first_buffer_timeout_ms = 5000;    // 第一个 Buffer 超时时间
        
        // ========== PSNR 对比配置（可选）==========
        bool enable_psnr_compare = false;      // 是否启用 PSNR 对比（自动创建软件解码器作为参考）
        double quick_psnr_threshold = 38.0;     // PSNR 快速阈值
        double quick_warn_threshold = 35.0;      // PSNR 警告阈值
        double ssim_threshold = 0.95;           // SSIM 阈值
        double ssim_warn_threshold = 0.90;      // SSIM 警告阈值
        bool enable_parallel = true;            // 是否启用并行计算
        bool use_perceptual_weighting = true;    // 是否使用感知加权
        bool save_psnr_report = false;           // 是否保存 PSNR 报告
        std::string psnr_report_path = "./psnr_compare_report.txt";  // PSNR 报告路径（单通道模式）
        std::string psnr_report_path_ch0 = "./psnr_compare_ch0_report.txt";  // PSNR 报告路径（ch0，多通道模式）
        std::string psnr_report_path_ch1 = "./psnr_compare_ch1_report.txt";  // PSNR 报告路径（ch1，多通道模式）
        bool enable_multi_channel_psnr = false;   // 是否启用多通道 PSNR 对比（分别统计 PP0 和 PP1）
        bool enable_pts_alignment = true;        // 是否启用 PTS 对齐（用于匹配硬件和软件解码器的帧）
        int max_pts_match_attempts = 10;        // PTS 匹配最大尝试次数
        
        // ========== 解码器诊断配置（可选）==========
        bool enable_decoder_verification = true; // 是否启用解码器验证和诊断（检查硬件解码器是否真正使用）
        bool verbose_diagnosis = false;          // 是否输出详细的诊断信息（包括可能的错误原因）
    };
    
    /**
     * @brief 统计信息
     */
    struct Stats {
        // 使用 std::atomic 支持线程安全（如果未来需要多线程消费）
        std::atomic<int> total_consumed{0};      // 总消费数量
        std::atomic<int> success_count{0};        // 成功数量
        std::atomic<int> failed_count{0};         // 失败数量
        std::atomic<int> skipped_count{0};        // 跳过数量（通道未启用等）
        std::atomic<int> drained_count{0};        // 排空数量
        std::atomic<double> avg_fps{0.0};         // 平均帧率
        
        // 按通道统计（可选，用于调试）
        std::atomic<int> ch0_consumed{0};          // ch0 消费数量
        std::atomic<int> ch1_consumed{0};          // ch1 消费数量
        std::atomic<int> ch0_skipped{0};           // ch0 跳过数量
        std::atomic<int> ch1_skipped{0};           // ch1 跳过数量
    };
    
    BufferConsumerService();
    ~BufferConsumerService();
    
    // 禁止拷贝
    BufferConsumerService(const BufferConsumerService&) = delete;
    BufferConsumerService& operator=(const BufferConsumerService&) = delete;

    // 兼容旧代码：在类内提供 RunOptions 别名，等价于命名空间级的 RunOptions
    using RunOptions = ::productionline::consumer::RunOptions;
    
    /**
     * @brief 打开服务（初始化生产线和 BufferPool）
     * @param config 服务配置
     * @param consumer 消费者实例（由调用者管理生命周期）
     * @param error_callback 错误回调（可选）
     * @return true 成功，false 失败
     */
    bool open(const Config& config, 
              IBufferConsumer* consumer,
              std::function<void(const std::string&)> error_callback = nullptr);
    
    /**
     * @brief 运行消费循环（阻塞直到停止或完成）
     * @param running_flag 运行标志（外部控制，设置为 false 时停止）
     */
    void run(std::atomic<bool>& running_flag);
    
    /**
     * @brief 一键运行：open → run → printStats → close
     *
     * @param config 服务配置
     * @param consumer 消费者实例（由调用者管理生命周期）
     * @param options 运行选项（可选）
     * @return true 成功运行完成，false 打开失败
     *
     * @note 该方法是对现有 open/run/close 的封装，不改变原有行为。
     */
    bool runOnce(const Config& config,
                 IBufferConsumer* consumer,
                 RunOptions options = RunOptions());
    
    /**
     * @brief 关闭服务（停止生产线，清理资源）
     */
    void close();
    
    /**
     * @brief 获取生产线实例（用于高级操作，如获取统计信息）
     */
    VideoProductionLine* getProductionLine() { 
        return is_open_ ? producer_.get() : nullptr; 
    }
    
    /**
     * @brief 获取 BufferPool（用于高级操作）
     */
    std::shared_ptr<BufferPool> getBufferPool() { 
        return is_open_ ? pool_sptr_ : nullptr; 
    }
    
    /**
     * @brief 获取统计信息（返回引用，避免复制 std::atomic）
     */
    const Stats& getStats() const { return stats_; }
    
    /**
     * @brief 打印统计摘要（类似 BufferWriter::printStats）
     */
    void printStats() const;
    
    /**
     * @brief 是否已打开
     */
    bool isOpen() const { return is_open_; }
    
    /**
     * @brief 获取指定通道的 PSNR Comparator（用于获取统计信息）
     * @param channel_id 通道 ID（0=PP0, 1=PP1）
     * @return Comparator 指针，如果未启用或通道不存在则返回 nullptr
     */
    io::BufferComparator* getPSNRComparator(int channel_id) const;

private:
    Config config_;
    bool is_open_;
    IBufferConsumer* consumer_;
    std::unique_ptr<VideoProductionLine> producer_;
    std::shared_ptr<BufferPool> pool_sptr_;
    Stats stats_;
    
    // PSNR 对比相关（可选）
    std::unique_ptr<VideoProductionLine> sw_producer_;  // 软件解码器（作为参考）
    std::shared_ptr<BufferPool> sw_pool_sptr_;         // 软件解码器的 BufferPool
    std::unique_ptr<io::BufferComparator> psnr_comparator_;  // PSNR 对比器（单通道模式）
    std::map<int, std::unique_ptr<io::BufferComparator>> psnr_comparators_;  // PSNR 对比器（多通道模式，key=channel_id）
    bool psnr_initialized_;
    // ⭐ 多通道模式：硬件buffer等待队列（按PTS索引，等待同一PTS的所有通道buffer到达）
    struct PendingHwBuffers {
        std::map<int, Buffer*> channel_buffers;  // channel_id -> 硬件buffer
        std::set<int> arrived_channels;  // 已到达的通道集合
    };
    std::map<int64_t, PendingHwBuffers> pending_hw_buffers_;  // PTS -> 等待的硬件buffer
    std::mutex pending_hw_buffers_mutex_;  // 保护等待队列的互斥锁
    std::atomic<int> psnr_compare_count_{0};  // 总对比次数（线程安全）
    std::atomic<int> psnr_pass_count_{0};      // 通过次数（线程安全）
    std::atomic<int> psnr_warn_count_{0};      // 警告次数（线程安全）
    std::atomic<int> psnr_fail_count_{0};      // 失败次数（线程安全）
    // 按通道的 PSNR 统计（多通道模式）
    std::map<int, std::atomic<int>> psnr_compare_count_by_channel_;  // 每个通道的对比次数
    std::map<int, std::atomic<int>> psnr_pass_count_by_channel_;      // 每个通道的通过次数
    std::map<int, std::atomic<int>> psnr_warn_count_by_channel_;      // 每个通道的警告次数
    std::map<int, std::atomic<int>> psnr_fail_count_by_channel_;      // 每个通道的失败次数
    log4cplus::Logger logger_;
    
    bool initializeProducer(std::function<void(const std::string&)> error_callback);
    bool initializeBufferPool();
    bool initializeConsumer();
    bool initializePSNRCompare(std::function<void(const std::string&)> error_callback);
    bool performPSNRCompare(Buffer* hw_buffer);
    Buffer* acquireSoftwareBufferByPTS(int64_t hw_pts);  // 通过PTS获取软件buffer（辅助函数）
    void processPendingBuffers();  // 处理等待队列中剩余的buffer（当producer停止或超时时）
    void verifyDecoders();  // 验证硬件和软件解码器（诊断功能）
    void consumeLoop(std::atomic<bool>& running_flag);
    void drainRemainingBuffers();
    
    // ========== 内部辅助方法（拆分长函数，提升可读性/可复用性）==========
    
    /**
     * @brief 处理单个 Buffer 的完整消费流程（通道检查 → 可选 PSNR → 消费 → 统计）
     * @param buffer 待处理的 Buffer（filled）
     * @param from_drain 是否来自排空阶段（影响 drained_count 统计）
     *
     * @note 该方法内部会根据 performPSNRCompare 的返回值决定是否立即消费/释放 Buffer，
     *       保持与原有逻辑完全一致。
     */
    void processSingleBuffer(Buffer* buffer, bool from_drain);
};

/**
 * @brief BufferConsumerService 配置构建器
 * 
 * 提供链式调用的方式构建 BufferConsumerService::Config
 * 
 * @example
 * ```cpp
 * auto config = ConsumerConfigBuilder()
 *     .setWorkerConfig(workerConfig)
 *     .setLoop(false)
 *     .setThreadCount(1)
 *     .setMaxFrames(300)
 *     .setEnablePSNRCompare(true)
 *     .setQuickPSNRThreshold(38.0)
 *     .build();
 * ```
 */
class ConsumerConfigBuilder {
public:
    ConsumerConfigBuilder() = default;
    
    // ========== 基础配置 ==========
    
    /**
     * @brief 设置 Worker 配置（必需）
     */
    ConsumerConfigBuilder& setWorkerConfig(const WorkerConfig& worker_config);
    
    /**
     * @brief 设置是否循环播放
     */
    ConsumerConfigBuilder& setLoop(bool loop);
    
    /**
     * @brief 设置线程数
     */
    ConsumerConfigBuilder& setThreadCount(int thread_count);
    
    /**
     * @brief 设置是否启用监控
     */
    ConsumerConfigBuilder& setEnableMonitor(bool enable);
    
    /**
     * @brief 设置最大帧数（-1表示无限制）
     */
    ConsumerConfigBuilder& setMaxFrames(int max_frames);
    
    // ========== 超时和排空配置 ==========
    
    /**
     * @brief 设置获取 Buffer 超时时间（毫秒）
     */
    ConsumerConfigBuilder& setAcquireTimeout(int timeout_ms);
    
    /**
     * @brief 设置最大超时次数
     */
    ConsumerConfigBuilder& setMaxTimeoutCount(int count);
    
    /**
     * @brief 设置是否排空剩余 Buffer
     */
    ConsumerConfigBuilder& setDrainRemaining(bool drain);
    
    /**
     * @brief 设置是否等待第一个 Buffer
     */
    ConsumerConfigBuilder& setWaitFirstBuffer(bool wait);
    
    /**
     * @brief 设置第一个 Buffer 超时时间（毫秒）
     */
    ConsumerConfigBuilder& setFirstBufferTimeout(int timeout_ms);
    
    // ========== PSNR 对比配置 ==========
    
    /**
     * @brief 设置是否启用 PSNR 对比
     */
    ConsumerConfigBuilder& setEnablePSNRCompare(bool enable);
    
    /**
     * @brief 设置是否启用多通道 PSNR 对比
     */
    ConsumerConfigBuilder& setEnableMultiChannelPSNR(bool enable);
    
    /**
     * @brief 设置 PSNR 快速阈值
     */
    ConsumerConfigBuilder& setQuickPSNRThreshold(double threshold);
    
    /**
     * @brief 设置 PSNR 警告阈值
     */
    ConsumerConfigBuilder& setQuickWarnThreshold(double threshold);
    
    /**
     * @brief 设置 SSIM 阈值
     */
    ConsumerConfigBuilder& setSSIMThreshold(double threshold);
    
    /**
     * @brief 设置 SSIM 警告阈值
     */
    ConsumerConfigBuilder& setSSIMWarnThreshold(double threshold);
    
    /**
     * @brief 设置是否启用并行计算
     */
    ConsumerConfigBuilder& setEnableParallel(bool enable);
    
    /**
     * @brief 设置是否使用感知加权
     */
    ConsumerConfigBuilder& setUsePerceptualWeighting(bool enable);
    
    /**
     * @brief 设置是否保存 PSNR 报告
     */
    ConsumerConfigBuilder& setSavePSNRReport(bool save);
    
    /**
     * @brief 设置 PSNR 报告路径（单通道模式）
     */
    ConsumerConfigBuilder& setPSNRReportPath(const std::string& path);
    
    /**
     * @brief 设置 PSNR 报告路径（多通道模式 - ch0）
     */
    ConsumerConfigBuilder& setPSNRReportPathCh0(const std::string& path);
    
    /**
     * @brief 设置 PSNR 报告路径（多通道模式 - ch1）
     */
    ConsumerConfigBuilder& setPSNRReportPathCh1(const std::string& path);
    
    /**
     * @brief 设置是否启用 PTS 对齐
     */
    ConsumerConfigBuilder& setEnablePTSAlignment(bool enable);
    
    /**
     * @brief 设置 PTS 匹配最大尝试次数
     */
    ConsumerConfigBuilder& setMaxPTSMatchAttempts(int attempts);
    
    /**
     * @brief 设置是否启用解码器验证
     */
    ConsumerConfigBuilder& setEnableDecoderVerification(bool enable);
    
    /**
     * @brief 设置是否输出详细诊断信息
     */
    ConsumerConfigBuilder& setVerboseDiagnosis(bool verbose);
    
    /**
     * @brief 构建最终配置
     */
    BufferConsumerService::Config build() const;
    
private:
    BufferConsumerService::Config config_;
};

// ========== 具体消费者实现 ==========

/**
 * @brief 显示消费者（DMA 显示）
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

/**
 * @brief 双BufferPool对比服务（支持PTS对齐）
 * 
 * 用于对比两个解码器的输出（硬件 vs 软件），支持PTS对齐。
 * 
 * 设计特点：
 * - 同时从两个BufferPool获取Buffer
 * - 自动实现PTS对齐（确保比较的是同一时刻的帧）
 * - 处理超时和错误情况
 * - 支持帧类型统计和诊断
 * 
 * 使用方式：
 * ```cpp
 * DualBufferCompareService service;
 * service.setComparator(&comparator);
 * service.setReferencePool(sw_pool);  // 软件解码器作为参考
 * service.setTestPool(hw_pool);       // 硬件解码器作为测试
 * 
 * DualBufferCompareService::Config config;
 * config.max_frames = 300;
 * config.acquire_timeout_ms = 100;
 * config.max_timeout_count = 50;
 * config.enable_pts_alignment = true;
 * 
 * if (service.open(config)) {
 *     service.run(running_flag);
 *     service.printStats();
 *     service.close();
 * }
 * ```
 */
class DualBufferCompareService {
public:
    /**
     * @brief 服务配置
     */
    struct Config {
        int max_frames = -1;                    // 最大帧数（-1表示无限制）
        int acquire_timeout_ms = 100;           // 获取Buffer超时时间（毫秒）
        int max_timeout_count = 50;             // 最大超时次数
        bool enable_pts_alignment = true;       // 是否启用PTS对齐
        int max_pts_match_attempts = 10;        // PTS匹配最大尝试次数
        bool drain_remaining = true;            // 是否排空剩余Buffer
        bool verbose = false;                    // 是否详细日志
        
        // ========== 裁剪配置 ==========
        bool enable_crop = false;               // 是否启用裁剪
        int crop_x = 0;                         // 裁剪起始X坐标
        int crop_y = 0;                         // 裁剪起始Y坐标
        int crop_w = 0;                         // 裁剪宽度（0表示不裁剪）
        int crop_h = 0;                         // 裁剪高度（0表示不裁剪）
    };
    
    /**
     * @brief 统计信息
     */
    struct Stats {
        int total_compared = 0;                 // 总对比数量
        int passed_count = 0;                   // 通过数量
        int warned_count = 0;                   // 警告数量
        int failed_count = 0;                   // 失败数量
        int timeout_count = 0;                  // 超时次数
        int pts_alignment_failures = 0;         // PTS对齐失败次数
        int frame_type_mismatches = 0;          // 帧类型不匹配次数
        std::vector<double> psnr_y_values;     // PSNR-Y值列表
        std::vector<double> psnr_avg_values;   // PSNR-Avg值列表
        std::vector<double> ssim_y_values;     // SSIM-Y值列表
        std::vector<double> ssim_avg_values;   // SSIM-Avg值列表
    };
    
    DualBufferCompareService();
    ~DualBufferCompareService();
    
    // 禁止拷贝
    DualBufferCompareService(const DualBufferCompareService&) = delete;
    DualBufferCompareService& operator=(const DualBufferCompareService&) = delete;
    
    /**
     * @brief 设置比较器
     */
    void setComparator(io::BufferComparator* comparator) {
        comparator_ = comparator;
    }
    
    /**
     * @brief 设置参考BufferPool（通常是软件解码器）
     */
    void setReferencePool(std::shared_ptr<BufferPool> pool) {
        reference_pool_ = pool;
    }
    
    /**
     * @brief 设置测试BufferPool（通常是硬件解码器）
     */
    void setTestPool(std::shared_ptr<BufferPool> pool) {
        test_pool_ = pool;
    }
    
    /**
     * @brief 设置参考生产线（用于检查运行状态）
     */
    void setReferenceProducer(VideoProductionLine* producer) {
        reference_producer_ = producer;
    }
    
    /**
     * @brief 设置测试生产线（用于检查运行状态）
     */
    void setTestProducer(VideoProductionLine* producer) {
        test_producer_ = producer;
    }
    
    /**
     * @brief 打开服务（验证配置）
     * @param config 服务配置
     * @return true 成功，false 失败
     */
    bool open(const Config& config);
    
    /**
     * @brief 运行对比循环（阻塞直到停止或完成）
     * @param running_flag 运行标志（外部控制，设置为 false 时停止）
     */
    void run(std::atomic<bool>& running_flag);
    
    /**
     * @brief 关闭服务（清理资源）
     */
    void close();
    
    /**
     * @brief 获取统计信息
     */
    Stats getStats() const { return stats_; }
    
    /**
     * @brief 打印统计摘要
     */
    void printStats() const;
    
    /**
     * @brief 是否已打开
     */
    bool isOpen() const { return is_open_; }

private:
    Config config_;
    bool is_open_;
    io::BufferComparator* comparator_;
    std::shared_ptr<BufferPool> reference_pool_;
    std::shared_ptr<BufferPool> test_pool_;
    VideoProductionLine* reference_producer_;
    VideoProductionLine* test_producer_;
    Stats stats_;
    log4cplus::Logger logger_;
    
    /**
     * @brief 从两个BufferPool获取PTS对齐的Buffer对
     * @param[out] ref_buf 参考Buffer（输出）
     * @param[out] test_buf 测试Buffer（输出）
     * @param acquire_timeout 获取超时时间（毫秒）
     * @return true 成功获取对齐的Buffer对，false 失败或超时
     */
    bool acquireAlignedBuffers(Buffer*& ref_buf, Buffer*& test_buf, int acquire_timeout);
    
    /**
     * @brief 获取Buffer的PTS值
     */
    int64_t getBufferPTS(Buffer* buffer);
    
    /**
     * @brief 裁剪Buffer区域（用于裁剪后的PSNR计算）
     * @param buffer 源Buffer
     * @param crop_x 裁剪起始X坐标
     * @param crop_y 裁剪起始Y坐标
     * @param crop_w 裁剪宽度
     * @param crop_h 裁剪高度
     * @return 裁剪后的AVFrame（需要调用者释放），失败返回nullptr
     */
    AVFrame* cropBufferRegion(Buffer* buffer, int crop_x, int crop_y, int crop_w, int crop_h);
    
    /**
     * @brief 创建裁剪后的临时Buffer（用于对比）
     * @param src_buffer 源Buffer
     * @param crop_x 裁剪起始X坐标
     * @param crop_y 裁剪起始Y坐标
     * @param crop_w 裁剪宽度
     * @param crop_h 裁剪高度
     * @return 裁剪后的Buffer（需要调用者释放），失败返回nullptr
     * 
     * 注意：此函数会创建一个临时的Buffer，使用完毕后需要释放。
     * 由于Buffer的创建比较复杂，这里简化实现：返回nullptr，实际对比时使用裁剪后的AVFrame。
     */
    Buffer* createCroppedBuffer(Buffer* src_buffer, int crop_x, int crop_y, int crop_w, int crop_h);
    
    /**
     * @brief 排空剩余Buffer
     */
    void drainRemainingBuffers();
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

} // namespace consumer
} // namespace productionline
