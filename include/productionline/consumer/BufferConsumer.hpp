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
#include <sstream>
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
 * @brief 错误码枚举（统一错误处理）
 * 
 * 用于标识不同类型的错误，便于错误处理和诊断
 */
enum class ConsumerErrorCode {
    SUCCESS = 0,                    // 成功
    
    // ========== 初始化错误 (1000-1999) ==========
    ERROR_INVALID_CONFIG = 1001,    // 配置无效
    ERROR_CONSUMER_NULL = 1002,     // 消费者指针为空
    ERROR_ALREADY_OPEN = 1003,      // 服务已打开
    ERROR_NOT_OPEN = 1004,          // 服务未打开
    
    // ========== 生产者错误 (2000-2999) ==========
    ERROR_PRODUCER_START_FAILED = 2001,  // 生产者启动失败
    ERROR_PRODUCER_NOT_RUNNING = 2002,   // 生产者未运行
    
    // ========== BufferPool 错误 (3000-3999) ==========
    ERROR_BUFFER_POOL_NOT_FOUND = 3001,  // BufferPool 未找到
    ERROR_BUFFER_POOL_DESTROYED = 3002,  // BufferPool 已销毁
    ERROR_BUFFER_ACQUIRE_TIMEOUT = 3003, // Buffer 获取超时
    ERROR_FIRST_BUFFER_TIMEOUT = 3004,   // 第一个 Buffer 获取超时
    
    // ========== 消费者错误 (4000-4999) ==========
    ERROR_CONSUMER_INIT_FAILED = 4001,   // 消费者初始化失败
    ERROR_CONSUMER_CONSUME_FAILED = 4002, // 消费者消费失败
    
    // ========== PSNR 对比错误 (5000-5999) ==========
    ERROR_PSNR_INIT_FAILED = 5001,      // PSNR 初始化失败
    ERROR_PSNR_SW_PRODUCER_FAILED = 5002, // 软件解码器创建失败
    ERROR_PSNR_COMPARATOR_FAILED = 5003,  // PSNR 对比器创建失败
    ERROR_PSNR_PTS_ALIGNMENT_FAILED = 5004, // PTS 对齐失败
    
    // ========== 运行时错误 (6000-6999) ==========
    ERROR_RUNTIME_TIMEOUT = 6001,        // 运行时超时
    ERROR_RUNTIME_MAX_FRAMES_REACHED = 6002, // 达到最大帧数
    ERROR_RUNTIME_UNKNOWN = 6999          // 未知运行时错误
};

/**
 * @brief 错误信息结构（增强的错误回调）
 * 
 * 包含完整的错误信息，便于错误处理和诊断
 */
struct ConsumerErrorInfo {
    ConsumerErrorCode code;              // 错误码
    std::string message;                 // 错误消息
    std::string location;                // 错误位置（函数名、文件名等）
    int line = 0;                        // 错误行号（可选）
    std::map<std::string, std::string> context; // 错误上下文（键值对）
    
    /**
     * @brief 转换为字符串（用于日志输出）
     */
    std::string toString() const {
        std::ostringstream oss;
        oss << "[" << static_cast<int>(code) << "] " << message;
        if (!location.empty()) {
            oss << " @ " << location;
            if (line > 0) {
                oss << ":" << line;
            }
        }
        if (!context.empty()) {
            oss << " {";
            bool first = true;
            for (const auto& pair : context) {
                if (!first) oss << ", ";
                oss << pair.first << "=" << pair.second;
                first = false;
            }
            oss << "}";
        }
        return oss.str();
    }
};

/**
 * @brief 增强的错误回调函数类型
 * 
 * 使用 ConsumerErrorInfo 替代简单的字符串，提供更丰富的错误信息
 */
using ErrorCallback = std::function<void(const ConsumerErrorInfo&)>;

/**
 * @brief 兼容旧代码的简单错误回调函数类型
 * 
 * 用于向后兼容，自动转换为增强的错误回调
 */
using SimpleErrorCallback = std::function<void(const std::string&)>;

/**
 * @brief Buffer 消费者接口（第一部分：策略接口，策略模式）
 * 
 * 职责：
 * - 定义消费算法的抽象接口
 * - 使不同消费策略可互换
 * - 单一职责：只负责消费逻辑
 * 
 * 设计模式：
 * - 策略模式：定义一系列算法，使它们可互换
 * 
 * 设计原则：
 * - 单一职责：只负责消费逻辑
 * - 无状态：消费操作应该是无状态的（或通过构造函数注入状态）
 * - 开闭原则：对扩展开放，对修改关闭（新增策略无需修改上下文）
 * 
 * 具体策略实现：
 * - DisplayConsumer：显示策略
 * - FileWriterConsumer：单文件写入策略
 * - MultiChannelFileWriterConsumer：多通道文件写入策略
 * - EncodedStreamWriterConsumer：编码流写入策略
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
     * @brief 增强的错误回调（推荐使用）
     * 
     * @note 提供完整的错误信息，包括错误码、位置、上下文等
     */
    ErrorCallback error_callback = nullptr;
    
    /**
     * @brief 简单错误回调（向后兼容）
     * 
     * @note 如果设置了 error_callback，此字段将被忽略
     * @deprecated 建议使用 error_callback 替代
     */
    SimpleErrorCallback simple_error_callback = nullptr;
};

// ============================================================================
// 第二部分：策略选择配置部分（前置声明，供 BufferConsumerService 使用）
// ============================================================================

/**
 * @brief 消费者配置（策略选择配置）
 * 
 * 职责：
 * - 定义所有消费策略的配置参数
 * - 通过配置选择消费策略类型
 * - 提供策略创建所需的所有参数
 */
struct ConsumerConfig {
    /**
     * @brief 消费者类型枚举
     */
    enum class Type {
        DISPLAY,              // 显示消费
        FILE_WRITER,          // 单文件写入消费
        MULTI_CHANNEL_FILE,   // 多通道文件写入消费
        BUFFER_COMPARE,       // Buffer比较消费
        ENCODED_STREAM        // 编码流写入消费
    };
    
    Type type = Type::DISPLAY;
    
    // ========== DisplayConsumer 配置 ==========
    LinuxFramebufferDevice* display_device = nullptr;
    bool display_ch0_enable = true;
    bool display_ch1_enable = false;
    
    // ========== FileWriterConsumer 配置 ==========
    std::string file_output_path;
    bool file_ch0_enable = true;
    bool file_ch1_enable = false;
    
    // ========== MultiChannelFileWriterConsumer 配置 ==========
    std::vector<std::string> multi_file_output_paths;
    bool multi_file_ch0_enable = true;
    bool multi_file_ch1_enable = false;
    
    // ========== BufferCompareConsumer 配置 ==========
    std::shared_ptr<BufferPool> reference_pool;
    io::CompareConfig compare_config;
    bool compare_ch0_enable = true;
    bool compare_ch1_enable = false;
    
    // ========== EncodedStreamWriterConsumer 配置 ==========
    std::string encoded_output_path;
    const AVCodecParameters* codec_params = nullptr;
    AVRational time_base = {0, 0};
};

/**
 * @brief PSNR 对比配置（子配置）
 * 
 * 用于配置 PSNR 对比相关的参数
 */
struct PSNRConfig {
    bool enable = false;                      // 是否启用 PSNR 对比
    bool enable_multi_channel = false;        // 是否启用多通道 PSNR 对比
    double quick_psnr_threshold = 38.0;       // PSNR 快速阈值
    double quick_warn_threshold = 35.0;       // PSNR 警告阈值
    double ssim_threshold = 0.95;             // SSIM 阈值
    double ssim_warn_threshold = 0.90;       // SSIM 警告阈值
    bool enable_parallel = true;              // 是否启用并行计算
    bool use_perceptual_weighting = true;      // 是否使用感知加权
    bool save_report = false;                  // 是否保存 PSNR 报告
    std::string report_path = "./psnr_compare_report.txt";  // PSNR 报告路径（单通道模式）
    std::string report_path_ch0 = "./psnr_compare_ch0_report.txt";  // PSNR 报告路径（ch0，多通道模式）
    std::string report_path_ch1 = "./psnr_compare_ch1_report.txt";  // PSNR 报告路径（ch1，多通道模式）
    bool enable_pts_alignment = true;         // 是否启用 PTS 对齐
    int max_pts_match_attempts = 10;           // PTS 匹配最大尝试次数
    bool enable_decoder_verification = true;   // 是否启用解码器验证
    bool verbose_diagnosis = false;            // 是否输出详细诊断信息
};

/**
 * @brief 生产者配置（子配置）
 * 
 * 用于配置 VideoProductionLine 相关参数
 */
struct ProductionLineConfig {
    bool loop = false;                        // 是否循环播放
    int thread_count = 1;                     // 线程数
    bool enable_monitor = false;              // 是否启用监控
};

/**
 * @brief 运行时配置（子配置）
 * 
 * 用于配置运行时行为参数
 */
struct RuntimeConfig {
    int max_frames = -1;                      // 最大帧数（-1表示无限制）
    int acquire_timeout_ms = 100;             // 获取 Buffer 超时时间（毫秒）
    int max_timeout_count = 50;               // 最大超时次数
    bool drain_remaining = true;              // 是否排空剩余 Buffer
    bool wait_first_buffer = true;             // 是否等待第一个 Buffer
    int first_buffer_timeout_ms = 5000;       // 第一个 Buffer 超时时间
};

/**
 * @brief BufferConsumerService - 消费者服务类（第二部分：上下文，策略模式）
 * 
 * 职责：
 * - 使用策略接口执行消费流程
 * - 管理生产线、缓冲池、统计等基础设施
 * - 不感知具体策略实现
 * 
 * 设计模式：
 * - 策略模式：通过 IBufferConsumer* 持有策略实例，运行时动态选择消费算法
 * - 门面模式：简化复杂的消费流程
 * - 模板方法模式：定义固定的消费流程骨架
 * 
 * 使用方式：
 * 
 * 方式一：传统方式（手动创建策略实例）
 * ```cpp
 * DisplayConsumer consumer(&display, true, false);
 * auto config = ConsumerConfigBuilder()
 *     .setWorkerConfig(workerConfig)
 *     .build();
 * 
 * BufferConsumerService service;
 * if (service.open(config, &consumer)) {
 *     service.run(running_flag);
 *     service.close();
 * }
 * ```
 * 
 * 方式二：简化方式（配置驱动，自动创建策略实例）
 * ```cpp
 * auto config = ConsumerConfigBuilder()
 *     .setWorkerConfig(workerConfig)
 *     .setConsumerType(ConsumerType::DISPLAY)
 *     .setDisplayDevice(&display)
 *     .setDisplayChannels(true, false)
 *     .build();
 * 
 * BufferConsumerService service;
 * service.execute(config, &running);  // 内部自动创建策略实例
 * ```
 */
class BufferConsumerService {
public:
    /**
     * @brief 服务配置（优化后的分层配置结构）
     */
    struct Config {
        WorkerConfig worker_config;           // Worker 配置（必需）
        ProductionLineConfig production_line; // 生产者配置
        RuntimeConfig runtime;                 // 运行时配置
        PSNRConfig psnr;                      // PSNR 对比配置（可选）
        
        /**
         * @brief 验证配置有效性
         * @return 错误信息，如果配置有效则返回空字符串
         */
        std::string validate() const {
            // WorkerType::AUTO 是有效值，允许自动检测
            if (runtime.max_frames < -1) {
                return "RuntimeConfig is invalid: max_frames must be >= -1";
            }
            if (runtime.acquire_timeout_ms < 0) {
                return "RuntimeConfig is invalid: acquire_timeout_ms must be >= 0";
            }
            if (runtime.max_timeout_count < 0) {
                return "RuntimeConfig is invalid: max_timeout_count must be >= 0";
            }
            if (runtime.first_buffer_timeout_ms < 0) {
                return "RuntimeConfig is invalid: first_buffer_timeout_ms must be >= 0";
            }
            if (production_line.thread_count < 1) {
                return "ProductionLineConfig is invalid: thread_count must be >= 1";
            }
            if (psnr.enable && psnr.max_pts_match_attempts < 1) {
                return "PSNRConfig is invalid: max_pts_match_attempts must be >= 1";
            }
            return "";  // 配置有效
        }
        
        // ========== 向后兼容：保留旧字段的访问器 ==========
        // 这些访问器允许旧代码继续工作，同时使用新的分层结构
        
        bool getLoop() const { return production_line.loop; }
        void setLoop(bool loop) { production_line.loop = loop; }
        
        int getThreadCount() const { return production_line.thread_count; }
        void setThreadCount(int count) { production_line.thread_count = count; }
        
        bool getEnableMonitor() const { return production_line.enable_monitor; }
        void setEnableMonitor(bool enable) { production_line.enable_monitor = enable; }
        
        int getMaxFrames() const { return runtime.max_frames; }
        void setMaxFrames(int frames) { runtime.max_frames = frames; }
        
        int getAcquireTimeout() const { return runtime.acquire_timeout_ms; }
        void setAcquireTimeout(int timeout) { runtime.acquire_timeout_ms = timeout; }
        
        int getMaxTimeoutCount() const { return runtime.max_timeout_count; }
        void setMaxTimeoutCount(int count) { runtime.max_timeout_count = count; }
        
        bool getDrainRemaining() const { return runtime.drain_remaining; }
        void setDrainRemaining(bool drain) { runtime.drain_remaining = drain; }
        
        bool getWaitFirstBuffer() const { return runtime.wait_first_buffer; }
        void setWaitFirstBuffer(bool wait) { runtime.wait_first_buffer = wait; }
        
        int getFirstBufferTimeout() const { return runtime.first_buffer_timeout_ms; }
        void setFirstBufferTimeout(int timeout) { runtime.first_buffer_timeout_ms = timeout; }
        
        bool getEnablePSNRCompare() const { return psnr.enable; }
        void setEnablePSNRCompare(bool enable) { psnr.enable = enable; }
        
        // ... 更多兼容性访问器可以根据需要添加
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
     * @param error_callback 增强的错误回调（可选，推荐使用）
     * @return true 成功，false 失败
     */
    bool open(const Config& config, 
              IBufferConsumer* consumer,
              ErrorCallback error_callback = nullptr);
    
    /**
     * @brief 打开服务（兼容旧接口）
     * @param config 服务配置
     * @param consumer 消费者实例（由调用者管理生命周期）
     * @param simple_error_callback 简单错误回调（向后兼容）
     * @return true 成功，false 失败
     * @deprecated 建议使用带 ErrorCallback 的版本
     */
    bool open(const Config& config, 
              IBufferConsumer* consumer,
              SimpleErrorCallback simple_error_callback);
    
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
     * @brief 一键执行：自动创建策略实例 → open → run → printStats → close（新接口）
     * 
     * 职责：
     * - 通过 ConsumerFactory 创建策略实例
     * - 执行完整的消费流程
     * - 自动管理策略实例的生命周期
     * 
     * @param service_config 服务配置（WorkerConfig、PSNR、超时等）
     * @param consumer_config 消费者策略配置（类型和参数）
     * @param running_flag 运行标志（可选，如果为 nullptr 则内部创建）
     * @param error_callback 增强的错误回调（可选，推荐使用）
     * @return true 成功执行完成，false 失败
     * 
     * @note 这是配置驱动的简化接口，内部会自动创建策略实例
     * 
     * @example
     * ```cpp
     * BufferConsumerService service;
     * auto service_config = ConsumerConfigBuilder()
     *     .setWorkerConfig(workerConfig)
     *     .setMaxFrames(1000)
     *     .build();
     * 
     * auto consumer_config = ConsumerStrategyConfigBuilder()
     *     .setType(ConsumerConfig::Type::DISPLAY)
     *     .setDisplayDevice(&display)
     *     .setDisplayChannels(true, false)
     *     .build();
     * 
     * std::atomic<bool> running(true);
     * ErrorCallback error_handler = [](const ConsumerErrorInfo& error) {
     *     LOG_ERROR("Error: " << error.toString());
     * };
     * if (service.execute(service_config, consumer_config, &running, error_handler)) {
     *     LOG_INFO("✅ 执行成功");
     * }
     * ```
     */
    bool execute(const Config& service_config,
                 const ConsumerConfig& consumer_config,
                 std::atomic<bool>* running_flag = nullptr,
                 ErrorCallback error_callback = nullptr);
    
    /**
     * @brief 一键执行（兼容旧接口）
     * @deprecated 建议使用带 ErrorCallback 的版本
     */
    bool execute(const Config& service_config,
                 const ConsumerConfig& consumer_config,
                 std::atomic<bool>* running_flag,
                 SimpleErrorCallback simple_error_callback);
    
    
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
    ErrorCallback error_callback_;  // 增强的错误回调
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
    
    bool initializeProducer(ErrorCallback error_callback);
    bool initializeBufferPool();
    bool initializeConsumer();
    bool initializePSNRCompare(ErrorCallback error_callback);
    
    /**
     * @brief 报告错误（统一错误处理入口）
     * @param code 错误码
     * @param message 错误消息
     * @param location 错误位置（函数名等）
     * @param line 错误行号（可选）
     * @param context 错误上下文（可选）
     */
    void reportError(ConsumerErrorCode code,
                     const std::string& message,
                     const std::string& location = "",
                     int line = 0,
                     const std::map<std::string, std::string>& context = {});
    
    /**
     * @brief 将简单错误回调转换为增强错误回调
     */
    ErrorCallback wrapSimpleErrorCallback(SimpleErrorCallback simple_callback);
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
 * @brief BufferConsumerService 配置构建器（第三部分：配置/工厂）
 * 
 * 职责：
 * - Builder 模式：通过链式调用构建 BufferConsumerService::Config
 * - Factory 模式：根据配置创建策略实例
 * - 统一管理配置和策略创建逻辑
 * 
 * 设计模式：
 * - Builder 模式：链式构建配置，提高可读性
 * - Factory 模式：根据配置创建策略实例，封装创建逻辑
 * 
 * 使用方式：
 * 
 * 方式一：使用实例方法创建策略实例
 * ```cpp
 * auto builder = ConsumerConfigBuilder()
 *     .setWorkerConfig(workerConfig)
 *     .setConsumerType(ConsumerType::DISPLAY)
 *     .setDisplayDevice(&display)
 *     .setDisplayChannels(true, false);
 * 
 * auto consumer = builder.createConsumer();  // 从当前配置创建策略实例
 * auto config = builder.build();              // 构建配置
 * ```
 * 
 * 方式二：使用静态方法创建策略实例
 * ```cpp
 * auto config = ConsumerConfigBuilder()
 *     .setWorkerConfig(workerConfig)
 *     .setConsumerType(ConsumerType::DISPLAY)
 *     .setDisplayDevice(&display)
 *     .build();
 * 
 * auto consumer = ConsumerConfigBuilder::createConsumer(config);  // 从配置创建策略实例
 * ```
 * 
 * 方式三：配合 execute() 使用（推荐）
 * ```cpp
 * auto config = ConsumerConfigBuilder()
 *     .setWorkerConfig(workerConfig)
 *     .setConsumerType(ConsumerType::DISPLAY)
 *     .setDisplayDevice(&display)
 *     .build();
 * 
 * BufferConsumerService service;
 * service.execute(config, &running);  // 内部自动调用 createConsumer()
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
    
    // ========== 分层配置的便捷方法 ==========
    
    /**
     * @brief 设置生产者配置（链式构建）
     */
    ConsumerConfigBuilder& setProductionLine(const ProductionLineConfig& config);
    
    /**
     * @brief 设置运行时配置（链式构建）
     */
    ConsumerConfigBuilder& setRuntime(const RuntimeConfig& config);
    
    /**
     * @brief 设置 PSNR 配置（链式构建）
     */
    ConsumerConfigBuilder& setPSNR(const PSNRConfig& config);
    
    // ========== 配置验证 ==========
    
    /**
     * @brief 验证当前配置
     * @return 错误信息，如果配置有效则返回空字符串
     */
    std::string validate() const;
    
    /**
     * @brief 构建最终配置（带验证）
     * @param validate_config 是否在构建时验证配置（默认 true）
     * @return 配置对象
     * @throw std::runtime_error 如果配置无效且 validate_config=true
     */
    BufferConsumerService::Config build(bool validate_config = true) const;
    
    /**
     * @brief 构建最终配置（不验证，用于向后兼容）
     * @deprecated 建议使用 build(true) 进行验证
     */
    BufferConsumerService::Config buildUnsafe() const {
        return build(false);
    }
    
    // 注意：消费者配置已移至独立的 ConsumerConfig 和 ConsumerFactory
    // 请使用 ConsumerStrategyConfigBuilder 构建消费者配置，使用 ConsumerFactory 创建消费者
    
private:
    BufferConsumerService::Config config_;
};

// ========== 具体消费者实现 ==========

/**
 * @brief 显示消费者（DMA 显示）
 */
// ============================================================================
// 第三部分：策略实现部分（已移至独立的策略库文件）
// ============================================================================
// 策略实现类已移至 BufferConsumerStrategies.hpp/cpp
// 包含策略库头文件以使用策略实现类
#include "productionline/consumer/BufferConsumerStrategies.hpp"

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

// EncodedStreamWriterConsumer 已移至 BufferConsumerStrategies.hpp

// ============================================================================
// 第二部分：策略选择配置部分（ConsumerConfig 已在上面定义）
// ============================================================================

/**
 * @brief 消费者工厂（根据配置创建策略实例）
 * 
 * 职责：
 * - 根据 ConsumerConfig 创建对应的消费策略实例
 * - 隐藏策略创建细节
 * - 提供统一的创建接口
 */
class ConsumerFactory {
public:
    /**
     * @brief 根据配置创建消费者实例
     * @param config 消费者配置
     * @return 消费者实例（unique_ptr，由调用者管理）
     */
    static std::unique_ptr<IBufferConsumer> create(const ConsumerConfig& config);
    
    /**
     * @brief 创建显示消费者（便捷方法）
     */
    static std::unique_ptr<IBufferConsumer> createDisplayConsumer(
        LinuxFramebufferDevice* display,
        bool ch0_enable = true,
        bool ch1_enable = false);
    
    /**
     * @brief 创建文件写入消费者（便捷方法）
     */
    static std::unique_ptr<IBufferConsumer> createFileWriterConsumer(
        const std::string& output_path,
        bool ch0_enable = true,
        bool ch1_enable = false);
    
    /**
     * @brief 创建多通道文件写入消费者（便捷方法）
     */
    static std::unique_ptr<IBufferConsumer> createMultiChannelFileWriterConsumer(
        const std::vector<std::string>& output_paths,
        bool ch0_enable = true,
        bool ch1_enable = false);
    
    /**
     * @brief 创建Buffer比较消费者（便捷方法）
     */
    static std::unique_ptr<IBufferConsumer> createBufferCompareConsumer(
        std::shared_ptr<BufferPool> reference_pool,
        const io::CompareConfig& compare_config,
        bool ch0_enable = true,
        bool ch1_enable = false);
    
    /**
     * @brief 创建编码流写入消费者（便捷方法）
     */
    static std::unique_ptr<IBufferConsumer> createEncodedStreamConsumer(
        const std::string& output_path,
        const AVCodecParameters* codec_params,
        AVRational time_base);

private:
    static log4cplus::Logger logger_;
};

/**
 * @brief 消费者策略配置建造者（简化策略配置构建）
 * 
 * 注意：此 Builder 用于构建独立的消费者策略配置（ConsumerConfig），
 * 不包含 WorkerConfig 等服务配置。如需完整服务配置，请使用上方的 ConsumerConfigBuilder。
 */
class ConsumerStrategyConfigBuilder {
public:
    ConsumerStrategyConfigBuilder& setType(ConsumerConfig::Type type) {
        config_.type = type;
        return *this;
    }
    
    // DisplayConsumer 配置方法
    ConsumerStrategyConfigBuilder& setDisplayDevice(LinuxFramebufferDevice* device) {
        config_.display_device = device;
        return *this;
    }
    
    ConsumerStrategyConfigBuilder& setDisplayChannels(bool ch0, bool ch1) {
        config_.display_ch0_enable = ch0;
        config_.display_ch1_enable = ch1;
        return *this;
    }
    
    // FileWriterConsumer 配置方法
    ConsumerStrategyConfigBuilder& setFileOutputPath(const std::string& path) {
        config_.file_output_path = path;
        return *this;
    }
    
    ConsumerStrategyConfigBuilder& setFileChannels(bool ch0, bool ch1) {
        config_.file_ch0_enable = ch0;
        config_.file_ch1_enable = ch1;
        return *this;
    }
    
    // MultiChannelFileWriterConsumer 配置方法
    ConsumerStrategyConfigBuilder& setMultiFileOutputPaths(const std::vector<std::string>& paths) {
        config_.multi_file_output_paths = paths;
        return *this;
    }
    
    ConsumerStrategyConfigBuilder& setMultiFileChannels(bool ch0, bool ch1) {
        config_.multi_file_ch0_enable = ch0;
        config_.multi_file_ch1_enable = ch1;
        return *this;
    }
    
    // BufferCompareConsumer 配置方法
    ConsumerStrategyConfigBuilder& setReferencePool(std::shared_ptr<BufferPool> pool) {
        config_.reference_pool = pool;
        return *this;
    }
    
    ConsumerStrategyConfigBuilder& setCompareConfig(const io::CompareConfig& compare_config) {
        config_.compare_config = compare_config;
        return *this;
    }
    
    ConsumerStrategyConfigBuilder& setCompareChannels(bool ch0, bool ch1) {
        config_.compare_ch0_enable = ch0;
        config_.compare_ch1_enable = ch1;
        return *this;
    }
    
    // EncodedStreamWriterConsumer 配置方法
    ConsumerStrategyConfigBuilder& setEncodedOutputPath(const std::string& path) {
        config_.encoded_output_path = path;
        return *this;
    }
    
    ConsumerStrategyConfigBuilder& setEncodedCodecParams(const AVCodecParameters* params, AVRational time_base) {
        config_.codec_params = params;
        config_.time_base = time_base;
        return *this;
    }
    
    ConsumerConfig build() const {
        return config_;
    }

private:
    ConsumerConfig config_;
};

// ============================================================================
// 测试服务（使用消费者组件）
// ============================================================================

/**
 * @brief 生产线测试服务 - 封装固定流程，使用消费者组件
 * 
 * 职责：
 * - 封装重复的测试流程：创建生产线 → 启动 → 获取Buffer → 消费 → 停止
 * - 管理生产线和BufferPool生命周期
 * - 提供统一的消费循环逻辑
 */
class ProductionLineTestService {
public:
    struct Options {
        bool loop = false;                     // 是否循环
        int thread_count = 1;                  // 线程数
        bool enable_monitor = false;           // 是否启用监控
        int max_frames = -1;                   // 最大帧数（-1表示无限制）
        int acquire_timeout_ms = 100;         // 获取Buffer超时（毫秒）
        int max_timeout_count = 50;            // 最大超时次数
        bool drain_remaining = true;           // 是否排空剩余Buffer
        bool wait_first_buffer = false;        // 是否等待第一个Buffer
        int first_buffer_timeout_ms = 5000;    // 第一个Buffer超时
    };
    
    struct Result {
        bool success = false;
        int total_consumed = 0;
        int success_count = 0;
        int failed_count = 0;
        int skipped_count = 0;
        int timeout_count = 0;
        int drained_count = 0;
        double avg_fps = 0.0;
    };
    
    ProductionLineTestService();
    ~ProductionLineTestService();
    
    // 禁止拷贝
    ProductionLineTestService(const ProductionLineTestService&) = delete;
    ProductionLineTestService& operator=(const ProductionLineTestService&) = delete;
    
    /**
     * @brief 运行完整测试流程
     * 
     * @param worker_config Worker配置
     * @param consumer 消费者实例（由调用者管理生命周期）
     * @param running_flag 运行标志（可选）
     * @param options 运行选项（可选）
     * @param error_callback 错误回调（可选）
     * @return Result 运行结果
     */
    Result run(
        const WorkerConfig& worker_config,
        IBufferConsumer* consumer,
        std::atomic<bool>* running_flag = nullptr,
        const Options& options = Options(),
        std::function<void(const std::string&)> error_callback = nullptr
    );
    
    /**
     * @brief 获取生产线实例（用于高级操作）
     */
    VideoProductionLine* getProductionLine() { return producer_.get(); }
    
    /**
     * @brief 获取BufferPool实例（用于高级操作）
     */
    std::shared_ptr<BufferPool> getBufferPool() { return pool_sptr_; }

private:
    bool initializeProducer(const WorkerConfig& worker_config,
                           const Options& options,
                           std::function<void(const std::string&)> error_callback);
    bool initializeBufferPool();
    bool initializeConsumer(IBufferConsumer* consumer, const Options& options);
    void consumeLoop(const Options& options,
                    IBufferConsumer* consumer,
                    std::atomic<bool>& running_flag,
                    Result& result);
    void drainRemainingBuffers(IBufferConsumer* consumer, Result& result);
    
    std::unique_ptr<VideoProductionLine> producer_;
    std::shared_ptr<BufferPool> pool_sptr_;
    log4cplus::Logger logger_;
};

} // namespace consumer
} // namespace productionline
