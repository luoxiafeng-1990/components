/**
 * @file WriterTestSuite.hpp
 * @brief 帧写入测试套件
 * 
 * 封装所有帧写入相关的测试功能，包括：
 * - YUV 格式写入测试
 * - RGB 格式写入测试
 * - 批量格式测试
 * 
 * 架构设计：
 * - 与 BufferConsumerService 的 ExecuteMode 对齐
 * - 帧写入测试使用 ExecuteMode::SINGLE + CONSUME_SAVE_RAW
 * 
 * 使用示例：
 * @code
 * ./qa_cases writer --format nv12 --input video.mp4 --output /tmp/out.yuv
 * ./qa_cases writer --format argb888 --input video.mp4
 * ./qa_cases writer -h
 * @endcode
 * 
 * @version 4.0 - 重构为 ExecuteMode 风格
 */

#ifndef WRITER_TEST_SUITE_HPP
#define WRITER_TEST_SUITE_HPP

#include "../common/ITestModule.hpp"
#include "productionline/io/BufferConsumerService.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include <map>
#include <string>
#include <vector>
#include <log4cplus/logger.h>

namespace test {
namespace writer {

// 类型别名：使用新架构的 ConsumeResult
using TestResult = consumer::ConsumeResult;

/**
 * @brief Writer 测试参数
 */
struct WriterTestParams {
    OutputFormat format;    ///< 输出格式
    std::string description;///< 格式描述
    int width;              ///< 输出宽度
    int height;             ///< 输出高度
    int save_frames;        ///< 保存帧数
    
    WriterTestParams(
        OutputFormat fmt = OutputFormat::YUV_NV12,
        const std::string& desc = "NV12",
        int w = 1920, int h = 1080,
        int frames = 10
    ) : format(fmt), description(desc), width(w), height(h), save_frames(frames) {}
};

/**
 * @brief 帧写入测试套件
 * 
 * 实现 ITestModule 接口，提供完整的帧写入测试功能。
 * 
 * 架构设计：
 * - 所有帧写入测试使用 ExecuteMode::SINGLE
 * - 消费标志：CONSUME_COUNT | CONSUME_SAVE_RAW
 */
class WriterTestSuite : public common::ITestModule {
public:
    WriterTestSuite() = default;
    ~WriterTestSuite() override = default;
    
    // ========================================
    // ITestModule 接口实现
    // ========================================
    
    std::string getName() const override { return "writer"; }
    std::string getDescription() const override { return "帧写入测试"; }
    
    int run(int argc, char* argv[]) override;
    void printHelp() const override;
    void listTests() const override;
    std::vector<std::string> getTestNames() const override;
    
    // ========================================
    // 核心测试方法（与 ExecuteMode 对齐）
    // ========================================
    
    /**
     * @brief 单路消费测试（ExecuteMode::SINGLE + CONSUME_SAVE_RAW）
     * 
     * 帧写入测试统一入口
     * 
     * @param input_path 输入文件路径
     * @param params 写入参数
     * @param output_path 输出文件路径（可选，空则自动生成）
     * @return 测试结果
     */
    static TestResult runSingle(
        const std::string& input_path,
        const WriterTestParams& params,
        const std::string& output_path = ""
    );
    
    /**
     * @brief 获取预定义测试参数
     */
    static const std::map<std::string, WriterTestParams>& getPredefinedTests();
    
    /**
     * @brief 获取所有 RGB 格式列表
     */
    static const std::vector<std::pair<OutputFormat, std::string>>& getRgbFormats();
    
    /**
     * @brief 获取所有 YUV 格式列表
     */
    static const std::vector<std::pair<OutputFormat, std::string>>& getYuvFormats();

private:
    /**
     * @brief 解析命令行参数
     */
    bool parseArgs(int argc, char* argv[], WorkerConfig& config, WriterTestParams& params, std::string& output_path);
    
    /**
     * @brief 获取模块级日志实例
     */
    static log4cplus::Logger& getLogger();
};

} // namespace writer
} // namespace test

#endif // WRITER_TEST_SUITE_HPP
