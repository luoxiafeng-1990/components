/**
 * @file TestExecutor.hpp
 * @brief 测试执行器
 * 
 * 封装通用的测试执行流程，包括：
 * - VideoProductionLine 启动和停止
 * - BufferPool 获取和消费
 * - 统计信息收集
 * - 结果输出
 * 
 * @version 3.1
 */

#ifndef TEST_EXECUTOR_HPP
#define TEST_EXECUTOR_HPP

#include "productionline/worker/WorkerConfig.hpp"
#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include <log4cplus/logger.h>

namespace test {
namespace common {

/**
 * @brief 测试执行结果
 */
struct TestResult {
    bool success = false;               ///< 是否成功
    int frames_decoded = 0;             ///< 解码帧数
    int frames_saved = 0;               ///< 保存帧数
    int frames_displayed = 0;           ///< 显示帧数
    int64_t packets_recorded = 0;       ///< 录制 Packet 数（录制测试用）
    int64_t bytes_recorded = 0;         ///< 录制字节数
    double duration_seconds = 0.0;      ///< 耗时（秒）
    double average_fps = 0.0;           ///< 平均帧率
    bool fps_passed = true;             ///< FPS 是否达标
    
    // PSNR 质量验证
    bool psnr_passed = true;            ///< PSNR 是否达标
    double psnr_average = 0.0;          ///< 平均 PSNR（单位：dB）
    
    // SSIM 质量验证
    bool ssim_passed = true;            ///< SSIM 是否达标
    double ssim_average = 0.0;          ///< 平均 SSIM（范围：0.0-1.0）
    
    std::string output_file;            ///< 输出文件路径
    std::string error_message;          ///< 错误信息
    
    TestResult() = default;
};

/**
 * @brief 测试执行器
 * 
 * 封装测试执行的通用流程。
 * 
 * 使用示例：
 * @code
 * // 创建配置
 * auto config = WorkerConfigFactory::createH264Decode("/path/to/video.mp4");
 * config.consumer.max_frames = 300;
 * config.consumer.save_frames = -1;
 * config.consumer.target_fps = 30.0;
 * 
 * // 执行测试
 * auto result = TestExecutor::runDecode(config);
 * 
 * // 打印结果
 * TestExecutor::printResult("H264 Decode Test", result);
 * @endcode
 */
class TestExecutor {
public:
    /**
     * @brief 执行解码测试
     * 
     * @param config WorkerConfig（包含组件配置和测试参数）
     * @return 测试结果
     */
    static TestResult runDecode(const WorkerConfig& config);
    
    /**
     * @brief 执行录制测试
     * 
     * 将输入源（RTSP/文件）录制到输出文件。
     * 
     * @param config WorkerConfig（包含输入源配置）
     * @param output_path 输出文件路径（扩展名决定容器格式：mp4/mkv/mov/ts/flv/avi）
     * @param max_duration_seconds 最大录制时长（秒，-1=无限制）
     * @return 测试结果
     */
    static TestResult runRecord(
        const WorkerConfig& config,
        const std::string& output_path,
        double max_duration_seconds = -1
    );
    
    /**
     * @brief 执行 PSNR/SSIM 验证测试
     * 
     * 比较硬件解码和软件解码的输出，计算 PSNR/SSIM。
     * 
     * @param hw_config 硬件解码配置
     * @param sw_config 软件解码配置（用作参考）
     * @return 测试结果
     */
    static TestResult runPsnrValidation(
        const WorkerConfig& hw_config,
        const WorkerConfig& sw_config
    );
    
    /**
     * @brief 执行多 Worker 测试
     * 
     * 多个 Worker 同时从同一数据源解码。
     * 
     * @param configs Worker 配置列表
     * @return 测试结果（聚合所有 Worker 的结果）
     */
    static TestResult runMultiWorker(
        const std::vector<WorkerConfig>& configs
    );
    
    /**
     * @brief 打印测试结果
     * 
     * @param test_name 测试名称
     * @param result 测试结果
     */
    static void printResult(const std::string& test_name, const TestResult& result);
    
    /**
     * @brief 打印测试头
     * 
     * @param test_name 测试名称
     * @param config 配置信息
     */
    static void printHeader(const std::string& test_name, const WorkerConfig& config);
    
    /**
     * @brief 打印分隔线
     */
    static void printSeparator();
    
    /**
     * @brief 设置信号处理（Ctrl+C）
     */
    static void setupSignalHandler();
    
    /**
     * @brief 检查是否正在运行
     */
    static bool isRunning();
    
    /**
     * @brief 请求停止
     */
    static void requestStop();
    
    /**
     * @brief 重置运行状态
     */
    static void resetState();

private:
    static std::atomic<bool> g_running_;
    static std::atomic<bool> g_interrupted_;
    
    // 模块级日志
    static log4cplus::Logger& getLogger();
};

} // namespace common
} // namespace test

#endif // TEST_EXECUTOR_HPP
