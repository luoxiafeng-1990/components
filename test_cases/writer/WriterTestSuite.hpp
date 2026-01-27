/**
 * @file WriterTestSuite.hpp
 * @brief BufferWriter 测试套件
 * 
 * 对应原始测试：
 * - writer: 通用格式写入测试
 * - writer_all_rgb_formats: 12 种 RGB 格式写入测试
 * - writer_all_yuv_formats: 15 种 YUV 格式写入测试
 */

#ifndef WRITER_TEST_SUITE_HPP
#define WRITER_TEST_SUITE_HPP

#include "../common/ITestModule.hpp"
#include "../common/TestExecutor.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include <map>
#include <string>
#include <vector>
#include <log4cplus/logger.h>

namespace test {
namespace writer {

/**
 * @brief Writer 测试参数
 */
struct WriterTestParams {
    OutputFormat format;        // 输出格式
    int width;                  // 宽度
    int height;                 // 高度
    int save_frames;            // 保存帧数
    std::string description;    // 格式描述
    
    WriterTestParams() 
        : format(OutputFormat::YUV_NV12), width(1920), height(1080), 
          save_frames(10), description("NV12") {}
    
    WriterTestParams(OutputFormat fmt, const std::string& desc)
        : format(fmt), width(1920), height(1080), save_frames(10), description(desc) {}
    
    WriterTestParams(OutputFormat fmt, int w, int h, int frames, const std::string& desc)
        : format(fmt), width(w), height(h), save_frames(frames), description(desc) {}
};

/**
 * @brief BufferWriter 测试套件
 * 
 * 测试 BufferWriter 对各种格式的写入支持
 */
class WriterTestSuite : public common::ITestModule {
public:
    WriterTestSuite() = default;
    ~WriterTestSuite() override = default;
    
    // ITestModule 接口
    std::string getName() const override { return "writer"; }
    std::string getDescription() const override { 
        return "BufferWriter format tests (RGB/YUV output)"; 
    }
    int run(int argc, char* argv[]) override;
    void printHelp() const override;
    std::vector<std::string> getTestNames() const override;
    
    // 预定义测试
    static const std::map<std::string, WriterTestParams>& getPredefinedTests();
    
    // 核心测试方法
    
    /**
     * @brief 运行单格式写入测试
     */
    static common::TestResult runWriterTest(
        const std::string& input_path,
        const WriterTestParams& params,
        const std::string& output_path = ""
    );
    
    /**
     * @brief 运行所有 RGB 格式测试
     * 对应原始 writer_all_rgb_formats
     */
    static common::TestResult runAllRgbFormats(
        const std::string& input_path,
        const std::string& output_dir = "/tmp"
    );
    
    /**
     * @brief 运行所有 YUV 格式测试
     * 对应原始 writer_all_yuv_formats
     */
    static common::TestResult runAllYuvFormats(
        const std::string& input_path,
        const std::string& output_dir = "/tmp"
    );
    
private:
    bool parseArgs(int argc, char* argv[], WorkerConfig& config, 
                   WriterTestParams& params, std::string& output_path);
    void listTests() const;
    
    // RGB 格式列表
    static const std::vector<std::pair<OutputFormat, std::string>>& getRgbFormats();
    
    // YUV 格式列表
    static const std::vector<std::pair<OutputFormat, std::string>>& getYuvFormats();
    
    /**
     * @brief 获取模块级日志实例
     */
    static log4cplus::Logger& getLogger();
};

} // namespace writer
} // namespace test

#endif // WRITER_TEST_SUITE_HPP
