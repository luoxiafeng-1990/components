/**
 * @file PPTestSuite.hpp
 * @brief 后处理（PP）测试套件
 * 
 * 封装所有后处理相关的测试功能，包括：
 * - PP0（通道0）YUV 格式测试
 * - PP1（通道1）RGB/YUV 格式测试
 * - Multi-PP（双通道）测试
 * - 裁剪和缩放测试
 * 
 * 使用示例：
 * @code
 * ./qa_cases pp --format nv12 --channel pp0 --input video.mp4
 * ./qa_cases pp --format argb888 --channel pp1 --input video.mp4
 * ./qa_cases pp -h
 * @endcode
 * 
 * @version 3.1
 */

#ifndef PP_TEST_SUITE_HPP
#define PP_TEST_SUITE_HPP

#include "../common/ITestModule.hpp"
#include "../common/TestExecutor.hpp"
#include "productionline/worker/WorkerConfig.hpp"

#include <string>
#include <vector>
#include <map>
#include <log4cplus/logger.h>

namespace test {
namespace pp {

/**
 * @brief PP 测试参数
 */
struct PPTestParams {
    std::string channel;        ///< 通道 (pp0, pp1, multi)
    OutputFormat format;        ///< 输出格式
    OutputFormat pp1_format;    ///< PP1 格式（仅 multi 模式）
    int width;                  ///< 输出宽度
    int height;                 ///< 输出高度
    ColorStandard color_std;    ///< 颜色标准
    
    // 裁剪参数（可选）
    int crop_x;
    int crop_y;
    int crop_w;
    int crop_h;
    
    // 默认构造 - channel 为空表示未使用预定义测试
    PPTestParams()
        : channel(""), format(OutputFormat::YUV_NV12), pp1_format(OutputFormat::YUV_AUTO),
          width(1920), height(1080), color_std(ColorStandard::BT601),
          crop_x(0), crop_y(0), crop_w(0), crop_h(0) {}
    
    // 单通道构造（4参数）
    PPTestParams(
        const std::string& ch,
        OutputFormat fmt,
        int w, int h
    ) : channel(ch), format(fmt), pp1_format(OutputFormat::YUV_AUTO),
        width(w), height(h), color_std(ColorStandard::BT601),
        crop_x(0), crop_y(0), crop_w(0), crop_h(0) {}
    
    // 单通道构造（5参数，带 ColorStandard）
    PPTestParams(
        const std::string& ch,
        OutputFormat fmt,
        int w, int h,
        ColorStandard std
    ) : channel(ch), format(fmt), pp1_format(OutputFormat::YUV_AUTO),
        width(w), height(h), color_std(std),
        crop_x(0), crop_y(0), crop_w(0), crop_h(0) {}
    
    // 单通道构造（带裁剪参数，9参数）
    PPTestParams(
        const std::string& ch,
        OutputFormat fmt,
        int w, int h,
        ColorStandard std,
        int cx, int cy, int cw, int ch_
    ) : channel(ch), format(fmt), pp1_format(OutputFormat::YUV_AUTO),
        width(w), height(h), color_std(std),
        crop_x(cx), crop_y(cy), crop_w(cw), crop_h(ch_) {}
    
    // Multi-PP 构造（4参数）
    PPTestParams(
        OutputFormat pp0_fmt,
        OutputFormat pp1_fmt,
        int w, int h
    ) : channel("multi"), format(pp0_fmt), pp1_format(pp1_fmt),
        width(w), height(h), color_std(ColorStandard::BT601),
        crop_x(0), crop_y(0), crop_w(0), crop_h(0) {}
    
    // Multi-PP 构造（5参数，带 ColorStandard）
    PPTestParams(
        OutputFormat pp0_fmt,
        OutputFormat pp1_fmt,
        int w, int h,
        ColorStandard std
    ) : channel("multi"), format(pp0_fmt), pp1_format(pp1_fmt),
        width(w), height(h), color_std(std),
        crop_x(0), crop_y(0), crop_w(0), crop_h(0) {}
};

/**
 * @brief 后处理测试套件
 * 
 * 实现 ITestModule 接口，提供完整的后处理测试功能。
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
    // 核心测试方法（参数化设计）
    // ========================================
    
    /**
     * @brief 执行 PP 测试（通用入口）
     * 
     * @param path 视频文件路径
     * @param params 测试参数
     * @return 测试结果
     */
    static common::TestResult runPPTest(
        const std::string& path,
        const PPTestParams& params
    );
    
    /**
     * @brief 执行 PP0 格式测试
     */
    static common::TestResult runPP0Test(
        const std::string& path,
        OutputFormat format,
        int width = 1920, int height = 1080,
        ColorStandard color_std = ColorStandard::BT601
    );
    
    /**
     * @brief 执行 PP1 格式测试
     */
    static common::TestResult runPP1Test(
        const std::string& path,
        OutputFormat format,
        int width = 1920, int height = 1080,
        ColorStandard color_std = ColorStandard::BT601
    );
    
    /**
     * @brief 执行 Multi-PP 测试
     */
    static common::TestResult runMultiPPTest(
        const std::string& path,
        OutputFormat pp0_format,
        OutputFormat pp1_format,
        int width = 1920, int height = 1080,
        ColorStandard color_std = ColorStandard::BT601
    );
    
    /**
     * @brief 执行裁剪缩放测试
     */
    static common::TestResult runCropScaleTest(
        const std::string& path,
        int crop_x, int crop_y, int crop_w, int crop_h,
        int scale_w, int scale_h
    );
    
    /**
     * @brief 获取预定义测试参数
     */
    static const std::map<std::string, PPTestParams>& getPredefinedTests();

private:
    /**
     * @brief 解析命令行参数
     */
    bool parseArgs(int argc, char* argv[], WorkerConfig& config, PPTestParams& params);
    
    /**
     * @brief 执行预定义测试
     */
    int runPredefinedTest(const std::string& test_name, const std::string& path);
    
    /**
     * @brief 获取模块级日志实例
     */
    static log4cplus::Logger& getLogger();
};

} // namespace pp
} // namespace test

#endif // PP_TEST_SUITE_HPP
