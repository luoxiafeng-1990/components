/**
 * @file VdecTestSuite.hpp
 * @brief 视频解码测试套件
 * 
 * 封装所有视频解码相关的测试功能，包括：
 * - H.264/H.265/MJPEG 硬件解码
 * - 软件解码
 * - RTSP 流解码
 * - PSNR/SSIM 质量验证
 * 
 * 架构设计：
 * - 与 BufferConsumerService 的 ExecuteMode 对齐
 * - 测试方法直接映射到 SINGLE / COMPARE / PARALLEL 模式
 * 
 * 使用示例：
 * @code
 * ./qa_cases vdec --file video.mp4 --codec h264 --width 1920 --height 1080
 * ./qa_cases vdec --rtsp rtsp://192.168.1.100/stream
 * ./qa_cases vdec -h
 * @endcode
 * 
 * @version 4.0 - 重构为 ExecuteMode 风格
 */

#ifndef VDEC_TEST_SUITE_HPP
#define VDEC_TEST_SUITE_HPP

#include "../common/ITestModule.hpp"
#include "productionline/io/BufferConsumerService.hpp"
#include "productionline/worker/WorkerConfig.hpp"

#include <string>
#include <vector>
#include <map>
#include <log4cplus/logger.h>

namespace test {
namespace vdec {

// 类型别名：使用新架构的 ConsumeResult
using TestResult = consumer::ConsumeResult;

/**
 * @brief 解码测试参数
 */
struct DecodeTestParams {
    std::string codec;           ///< 编解码器 (h264, h265, mjpeg)
    int width;                   ///< 分辨率宽度
    int height;                  ///< 分辨率高度
    double fps;                  ///< 目标帧率
    std::string profile;         ///< profile (main, baseline, high)
    bool use_hardware;           ///< 是否使用硬件解码（默认 true）
    std::string predefined_name; ///< 匹配的预定义测试名称（空表示未使用预定义测试）
    
    DecodeTestParams(
        const std::string& c = "h264",
        int w = 1920, int h = 1080,
        double f = 30.0,
        const std::string& p = "main",
        bool hw = true
    ) : codec(c), width(w), height(h), fps(f), profile(p), use_hardware(hw) {}
    
    /// 是否使用了预定义测试
    bool isPredefined() const { return !predefined_name.empty(); }
};

/**
 * @brief 视频解码测试套件
 * 
 * 实现 ITestModule 接口，提供完整的视频解码测试功能。
 * 
 * 架构设计：
 * - 所有测试方法与 BufferConsumerService::ExecuteMode 对齐
 * - runSingle()   → ExecuteMode::SINGLE
 * - runCompare()  → ExecuteMode::COMPARE (PSNR/SSIM)
 * - runParallel() → ExecuteMode::PARALLEL (多线程/多Worker)
 */
class VdecTestSuite : public common::ITestModule {
public:
    VdecTestSuite() = default;
    ~VdecTestSuite() override = default;
    
    // ========================================
    // ITestModule 接口实现
    // ========================================
    
    std::string getName() const override { return "vdec"; }
    std::string getDescription() const override { return "视频解码测试"; }
    
    int run(int argc, char* argv[]) override;
    void printHelp() const override;
    void listTests() const override;
    std::vector<std::string> getTestNames() const override;
    
    // ========================================
    // 辅助方法
    // ========================================
    // 注：runSingle/runCompare/runParallel 已移至基类 ITestModule
    
    /**
     * @brief 获取预定义测试参数
     * 
     * @return 预定义测试名称到参数的映射
     */
    static const std::map<std::string, DecodeTestParams>& getPredefinedTests();
    
    /**
     * @brief 从 WorkerConfig 构建消费标志
     */
    static uint32_t buildConsumeFlags(const WorkerConfig& config);

private:
    /**
     * @brief 解析命令行参数
     */
    bool parseArgs(int argc, char* argv[], WorkerConfig& config, DecodeTestParams& params);
    
    /**
     * @brief 执行预定义测试
     */
    int runPredefinedTest(const std::string& test_name, const std::string& path);
    
};

} // namespace vdec
} // namespace test

#endif // VDEC_TEST_SUITE_HPP
