/**
 * @file PPTestSuite.hpp
 * @brief 后处理（PP）测试套件
 * 
 * 封装所有后处理相关的测试功能，包括：
 * - PP0（通道0）YUV 格式测试
 * - PP1（通道1）RGB/YUV 格式测试
 * - Multi-PP（双通道）测试
 * - 裁剪和缩放测试
 * - PSNR/SSIM 质量验证
 * 
 * 架构设计：
 * - 与 BufferConsumerService 的 ExecuteMode 对齐
 * - 测试方法直接映射到 SINGLE / COMPARE / PARALLEL 模式
 * 
 * 使用示例：
 * @code
 * ./qa_cases pp --format nv12 --channel pp0 --input video.mp4
 * ./qa_cases pp --format argb888 --channel pp1 --input video.mp4
 * ./qa_cases pp --psnr video.mp4              # COMPARE 模式
 * ./qa_cases pp -h
 * @endcode
 * 
 * @version 4.1 - 重构为 ExecuteMode 风格，与 VdecTestSuite 架构对齐
 */

#ifndef PP_TEST_SUITE_HPP
#define PP_TEST_SUITE_HPP

#include "../common/ITestModule.hpp"
#include "productionline/io/BufferConsumerService.hpp"
#include "productionline/worker/WorkerConfig.hpp"

#include <string>
#include <vector>
#include <map>
#include <log4cplus/logger.h>

namespace test {
namespace pp {

// 类型别名：使用新架构的 ConsumeResult
using TestResult = consumer::ConsumeResult;

/**
 * @brief PP 测试参数（可扩展多通道设计）
 * 
 * 设计原则：
 * - channels: 启用的通道列表，如 [0], [1], [0,1], [0,1,2]
 * - formats: 每个通道的输出格式，与 channels 一一对应
 * - 兼容原有 "pp0"/"pp1"/"multi" 字符串格式
 */
struct PPTestParams {
    std::vector<int> channels;           ///< 启用的通道列表
    std::vector<OutputFormat> formats;   ///< 每个通道的输出格式
    int width;                           ///< 输出宽度
    int height;                          ///< 输出高度
    ColorStandard color_std;             ///< 颜色标准
    
    // 裁剪参数（可选）
    int crop_x;
    int crop_y;
    int crop_w;
    int crop_h;
    
    // 解码方式
    bool use_hardware;                   ///< 是否使用硬件解码（默认 true）
    
    // ========================================
    // 辅助方法
    // ========================================
    
    /// 获取兼容的 channel 字符串（用于日志和旧代码兼容）
    std::string getChannelString() const {
        if (channels.empty()) return "";
        if (channels.size() == 1) {
            return "pp" + std::to_string(channels[0]);
        }
        return "multi";
    }
    
    /// 获取指定通道的格式（如果 formats 不够，使用最后一个或默认值）
    OutputFormat getFormat(size_t index) const {
        if (formats.empty()) return OutputFormat::YUV_NV12;
        if (index < formats.size()) return formats[index];
        return formats.back();  // 不够时使用最后一个
    }
    
    /// 是否为多通道模式
    bool isMultiChannel() const { return channels.size() > 1; }
    
    // ========================================
    // 构造函数（保持兼容性）
    // ========================================
    
    // 默认构造 - channels 为空表示未使用预定义测试
    PPTestParams()
        : channels(), formats({OutputFormat::YUV_NV12}),
          width(1920), height(1080), color_std(ColorStandard::BT601),
          crop_x(0), crop_y(0), crop_w(0), crop_h(0), use_hardware(true) {}
    
    // 单通道构造（4参数）- 兼容 {"pp0", NV12, 1920, 1080}
    PPTestParams(
        const std::string& ch,
        OutputFormat fmt,
        int w, int h
    ) : channels(parseChannelString(ch)), formats({fmt}),
        width(w), height(h), color_std(ColorStandard::BT601),
        crop_x(0), crop_y(0), crop_w(0), crop_h(0), use_hardware(true) {}
    
    // 单通道构造（5参数，带 ColorStandard）
    PPTestParams(
        const std::string& ch,
        OutputFormat fmt,
        int w, int h,
        ColorStandard std
    ) : channels(parseChannelString(ch)), formats({fmt}),
        width(w), height(h), color_std(std),
        crop_x(0), crop_y(0), crop_w(0), crop_h(0), use_hardware(true) {}
    
    // 单通道构造（带裁剪参数，9参数）
    PPTestParams(
        const std::string& ch,
        OutputFormat fmt,
        int w, int h,
        ColorStandard std,
        int cx, int cy, int cw, int ch_
    ) : channels(parseChannelString(ch)), formats({fmt}),
        width(w), height(h), color_std(std),
        crop_x(cx), crop_y(cy), crop_w(cw), crop_h(ch_), use_hardware(true) {}
    
    // Multi-PP 构造（4参数）- 兼容 {NV12, RGB888, 1920, 1080}
    PPTestParams(
        OutputFormat pp0_fmt,
        OutputFormat pp1_fmt,
        int w, int h
    ) : channels({0, 1}), formats({pp0_fmt, pp1_fmt}),
        width(w), height(h), color_std(ColorStandard::BT601),
        crop_x(0), crop_y(0), crop_w(0), crop_h(0), use_hardware(true) {}
    
    // Multi-PP 构造（5参数，带 ColorStandard）
    PPTestParams(
        OutputFormat pp0_fmt,
        OutputFormat pp1_fmt,
        int w, int h,
        ColorStandard std
    ) : channels({0, 1}), formats({pp0_fmt, pp1_fmt}),
        width(w), height(h), color_std(std),
        crop_x(0), crop_y(0), crop_w(0), crop_h(0), use_hardware(true) {}
    
private:
    /// 解析通道字符串（"pp0" → [0], "pp1" → [1], "multi" → [0,1]）
    static std::vector<int> parseChannelString(const std::string& ch) {
        if (ch == "pp0" || ch == "0") return {0};
        if (ch == "pp1" || ch == "1") return {1};
        if (ch == "multi" || ch == "0,1") return {0, 1};
        // 默认通道 0
        return {0};
    }
};

/**
 * @brief 后处理测试套件
 * 
 * 实现 ITestModule 接口，提供完整的后处理测试功能。
 * 
 * 架构设计：
 * - 所有测试方法与 BufferConsumerService::ExecuteMode 对齐
 * - runSingle()   → ExecuteMode::SINGLE
 * - runCompare()  → ExecuteMode::COMPARE (PSNR/SSIM)
 * - runParallel() → ExecuteMode::PARALLEL (多线程/多Worker)
 */
class PPTestSuite : public common::ITestModule {
public:
    PPTestSuite() = default;
    ~PPTestSuite() override = default;
    
    // ========================================
    // ITestModule 接口实现
    // ========================================
    
    std::string getName() const override { return "pp"; }
    std::string getDescription() const override { return "后处理格式测试"; }
    
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
    static const std::map<std::string, PPTestParams>& getPredefinedTests();
    
    /**
     * @brief 从 WorkerConfig 构建消费标志
     */
    static uint32_t buildConsumeFlags(const WorkerConfig& config);
    
    /**
     * @brief 从 PPTestParams 构建 WorkerConfig
     */
    static WorkerConfig buildConfig(const std::string& path, const PPTestParams& params);

private:
    /**
     * @brief 解析命令行参数
     */
    bool parseArgs(int argc, char* argv[], WorkerConfig& config, PPTestParams& params);
    
    /**
     * @brief 执行预定义测试
     */
    int runPredefinedTest(const std::string& test_name, const std::string& path);
    
};

} // namespace pp
} // namespace test

#endif // PP_TEST_SUITE_HPP
