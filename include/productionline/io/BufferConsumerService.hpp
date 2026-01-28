/**
 * @file BufferConsumerService.hpp
 * @brief Buffer 消费服务
 * 
 * 设计模式：门面模式（Facade Pattern）+ 策略模式（Strategy Pattern）
 * 
 * 职责：
 * - 统一管理生产线创建、BufferPool 获取、消费循环执行
 * - 支持三种执行模式：SINGLE、COMPARE、PARALLEL
 * - 支持消费类型叠加（DISPLAY | SAVE_RAW 等）
 * 
 * 对外接口：
 * - start(): 唯一的公开接口，根据 ExecuteMode 和 ConsumeTypeFlags 执行消费
 * 
 * 内部方法对应：
 * - SINGLE → startProductionLine()
 * - COMPARE → startProductionLinesCompare()
 * - PARALLEL → startProductionLinesParallel()
 */

#ifndef BUFFER_CONSUMER_SERVICE_HPP
#define BUFFER_CONSUMER_SERVICE_HPP

#include "productionline/io/IBufferConsumer.hpp"
#include "productionline/io/BufferConsumerStrategies.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include "productionline/VideoProductionLine.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "common/GlobalThreadPool.hpp"
#include "common/Logger.hpp"

#include <vector>
#include <memory>
#include <atomic>
#include <string>
#include <chrono>

namespace consumer {

/**
 * @brief 消费结果
 * 
 * 统一的测试结果结构，替代旧的 TestResult。
 * 支持解码、录制、质量验证等多种场景。
 */
struct ConsumeResult {
    bool success = false;               ///< 是否成功
    int frames_consumed = 0;            ///< 消费帧数（兼容旧的 frames_decoded）
    int frames_displayed = 0;           ///< 显示帧数
    int frames_saved = 0;               ///< 保存帧数
    double duration_seconds = 0;        ///< 执行时长（秒）
    double average_fps = 0;             ///< 平均帧率
    std::string output_file;            ///< 输出文件路径
    std::string error_message;          ///< 错误信息
    
    // ========================================
    // 录制模式结果（CONSUME_SAVE_ENCODED）
    // ========================================
    int64_t packets_recorded = 0;       ///< 录制 Packet 数
    int64_t bytes_recorded = 0;         ///< 录制字节数
    
    // ========================================
    // 性能验证结果
    // ========================================
    bool fps_passed = true;             ///< FPS 是否达标
    double target_fps = 0;              ///< 目标 FPS（用于验证）
    
    // ========================================
    // 质量验证结果（COMPARE 模式）
    // ========================================
    int frames_compared = 0;            ///< 对比帧数
    double psnr_average = 0;            ///< 平均 PSNR（单位：dB）
    double ssim_average = 0;            ///< 平均 SSIM（范围：0.0-1.0）
    bool psnr_passed = true;            ///< PSNR 是否达标
    bool ssim_passed = true;            ///< SSIM 是否达标
    bool compare_passed = true;         ///< 质量验证是否通过（综合 PSNR 和 SSIM）
    
    // ========================================
    // PARALLEL 模式：每个 Worker 的独立结果
    // ========================================
    std::vector<ConsumeResult> worker_results;
};

/**
 * @brief Buffer 消费服务
 * 
 * 统一管理：生产线创建、BufferPool 获取、消费循环执行
 */
class BufferConsumerService {
public:
    BufferConsumerService();
    ~BufferConsumerService();
    
    /**
     * @brief 设置线程池
     * @param pool 线程池指针（PARALLEL 模式使用）
     */
    void setThreadPool(std::shared_ptr<BS::thread_pool<>> pool);
    
    // ============================================================
    // 唯一对外接口
    // ============================================================
    
    /**
     * @brief 启动消费
     * 
     * @param configs Worker 配置列表
     *        - SINGLE: 1 个
     *        - COMPARE: 2+ 个（通常 HW + SW）
     *        - PARALLEL: N 个
     * @param mode 执行模式
     * @param consume_flags 消费类型标志（可叠加，如 CONSUME_DISPLAY | CONSUME_SAVE_RAW）
     * @return 消费结果
     * 
     * 所有参数（output_path, save_frames, min_psnr 等）从 WorkerConfig::ConsumerConfig 中获取
     */
    ConsumeResult start(
        const std::vector<WorkerConfig>& configs,
        ExecuteMode mode,
        uint32_t consume_flags
    );
    
    // ============================================================
    // 控制接口
    // ============================================================
    
    /**
     * @brief 请求停止
     */
    void requestStop();
    
    /**
     * @brief 是否正在运行
     */
    bool isRunning() const;
    
    // ============================================================
    // 工具方法
    // ============================================================
    
    /**
     * @brief 打印测试结果
     */
    static void printResult(const std::string& test_name, const ConsumeResult& result);
    
    /**
     * @brief 打印测试头部信息
     */
    static void printHeader(const std::string& test_name, const WorkerConfig& config);
    
private:
    // ============================================================
    // 内部方法（与 ExecuteMode 对应）
    // ============================================================
    
    /**
     * @brief SINGLE 模式：单路消费
     * 
     * 1. 创建 VideoProductionLine
     * 2. 获取 BufferPool
     * 3. 根据 consume_flags 创建策略
     * 4. 从线程池获取线程执行消费循环
     */
    ConsumeResult startProductionLine(
        const WorkerConfig& config,
        uint32_t consume_flags
    );
    
    /**
     * @brief COMPARE 模式：对比消费
     * 
     * 1. 创建 N 个 VideoProductionLine
     * 2. 获取 N 个 BufferPool
     * 3. 创建 CompareConsumer
     * 4. 同步获取 N 个 Buffer，传给 CompareConsumer
     */
    ConsumeResult startProductionLinesCompare(
        const std::vector<WorkerConfig>& configs
    );
    
    /**
     * @brief PARALLEL 模式：并行消费
     * 
     * 1. 为每个 config 调用 startProductionLine
     * 2. 使用线程池管理多个线程
     * 3. 等待所有线程完成，汇总结果
     */
    ConsumeResult startProductionLinesParallel(
        const std::vector<WorkerConfig>& configs,
        uint32_t consume_flags
    );
    
    // ============================================================
    // 辅助方法
    // ============================================================
    
    /**
     * @brief 根据 consume_flags 创建消费策略
     */
    std::shared_ptr<IBufferConsumer> createConsumerFromFlags(
        uint32_t flags,
        const WorkerConfig& config
    );
    
    /**
     * @brief 消费循环（单 BufferPool）
     */
    void consumeLoop(
        std::shared_ptr<BufferPool> pool,
        std::shared_ptr<IBufferConsumer> consumer,
        const WorkerConfig::ConsumerConfig& config,
        ConsumeResult& result
    );
    
    /**
     * @brief 消费循环（多 BufferPool 同步，COMPARE 模式）
     */
    void consumeLoopCompare(
        const std::vector<std::shared_ptr<BufferPool>>& pools,
        std::shared_ptr<IBufferConsumer> consumer,
        const WorkerConfig::ConsumerConfig& config,
        ConsumeResult& result
    );
    
    // ============================================================
    // 状态
    // ============================================================
    
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    
    // 线程池
    std::shared_ptr<BS::thread_pool<>> thread_pool_;
    
    // 日志
    log4cplus::Logger logger_;
};

} // namespace consumer

#endif // BUFFER_CONSUMER_SERVICE_HPP
