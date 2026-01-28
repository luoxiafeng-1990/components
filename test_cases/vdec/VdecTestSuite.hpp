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
 * 使用示例：
 * @code
 * ./qa_cases vdec --file video.mp4 --codec h264 --width 1920 --height 1080
 * ./qa_cases vdec --rtsp rtsp://192.168.1.100/stream
 * ./qa_cases vdec -h
 * @endcode
 * 
 * @version 3.1
 */

#ifndef VDEC_TEST_SUITE_HPP
#define VDEC_TEST_SUITE_HPP

#include "../common/ITestModule.hpp"
#include "../common/TestExecutor.hpp"
#include "productionline/worker/WorkerConfig.hpp"

#include <string>
#include <vector>
#include <map>
#include <log4cplus/logger.h>

namespace test {
namespace vdec {

/**
 * @brief 解码测试参数
 */
struct DecodeTestParams {
    std::string codec;      ///< 编解码器 (h264, h265, mjpeg, software)
    int width;              ///< 分辨率宽度
    int height;             ///< 分辨率高度
    double fps;             ///< 目标帧率
    std::string profile;    ///< profile (main, baseline, high)
    
    DecodeTestParams(
        const std::string& c = "h264",
        int w = 1920, int h = 1080,
        double f = 30.0,
        const std::string& p = "main"
    ) : codec(c), width(w), height(h), fps(f), profile(p) {}
};

/**
 * @brief 视频解码测试套件
 * 
 * 实现 ITestModule 接口，提供完整的视频解码测试功能。
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
    // 核心测试方法（参数化设计）
    // ========================================
    
    /**
     * @brief 执行解码测试（通用入口）
     * 
     * @param path 视频文件路径或 RTSP URL
     * @param params 测试参数
     * @return 测试结果
     */
    static common::TestResult runDecodeTest(
        const std::string& path,
        const DecodeTestParams& params
    );
    
    /**
     * @brief 执行解码测试（简化版）
     * 
     * @param path 视频文件路径
     * @param codec 编解码器 (h264, h265, mjpeg, software)
     * @param width 分辨率宽度
     * @param height 分辨率高度
     * @param target_fps 目标帧率
     * @return 测试结果
     */
    static common::TestResult runDecodeTest(
        const std::string& path,
        const std::string& codec,
        int width,
        int height,
        double target_fps
    );
    
    /**
     * @brief 执行质量验证测试（PSNR/SSIM）
     * 
     * @param path 视频文件路径
     * @param params 测试参数
     * @param reference_path 参考文件路径
     * @param enable_psnr 启用 PSNR
     * @param enable_ssim 启用 SSIM
     * @return 测试结果
     */
    static common::TestResult runQualityTest(
        const std::string& path,
        const DecodeTestParams& params,
        const std::string& reference_path,
        bool enable_psnr = true,
        bool enable_ssim = false
    );
    
    /**
     * @brief 执行多 Worker 测试
     * 
     * 对应原始 test.cpp 中的 multi_worker 测试
     * 同时运行硬件解码器和软件解码器，验证多 Worker 协同工作
     * 
     * @param path 视频文件路径或 RTSP URL
     * @param params 测试参数
     * @return 测试结果
     */
    static common::TestResult runMultiWorkerTest(
        const std::string& path,
        const DecodeTestParams& params
    );
    
    /**
     * @brief 执行多线程解码测试
     * 
     * 对应原始 test.cpp 中的 ffmpeg_multithread 测试
     * 
     * @param path 视频文件路径
     * @param params 测试参数
     * @param thread_count 线程数量
     * @return 测试结果
     */
    static common::TestResult runMultithreadTest(
        const std::string& path,
        const DecodeTestParams& params,
        int thread_count = 4
    );
    
    /**
     * @brief 获取预定义测试参数
     * 
     * @return 预定义测试名称到参数的映射
     */
    static const std::map<std::string, DecodeTestParams>& getPredefinedTests();

private:
    /**
     * @brief 解析命令行参数
     */
    bool parseArgs(int argc, char* argv[], WorkerConfig& config, DecodeTestParams& params);
    
    /**
     * @brief 执行预定义测试
     */
    int runPredefinedTest(const std::string& test_name, const std::string& path);
    
    /**
     * @brief 获取模块级日志实例
     */
    static log4cplus::Logger& getLogger();
};

} // namespace vdec
} // namespace test

#endif // VDEC_TEST_SUITE_HPP
