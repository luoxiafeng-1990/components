/**
 * @file RecordTestSuite.hpp
 * @brief 录制测试套件
 * 
 * 封装所有录制相关的测试功能，包括：
 * - RTSP 流录制
 * - 文件重封装
 * - 多格式输出
 * 
 * 架构设计：
 * - 与 BufferConsumerService 的 ExecuteMode 对齐
 * - 录制测试使用 ExecuteMode::SINGLE + CONSUME_SAVE_ENCODED
 * 
 * 使用示例：
 * @code
 * ./qa_cases record --input rtsp://192.168.1.100/stream --output /tmp/test.mp4
 * ./qa_cases record --input video.mp4 --output /tmp/remux.mkv
 * ./qa_cases record -h
 * @endcode
 * 
 * @version 4.0 - 重构为 ExecuteMode 风格
 */

#ifndef RECORD_TEST_SUITE_HPP
#define RECORD_TEST_SUITE_HPP

#include "../common/ITestModule.hpp"
#include "productionline/io/BufferConsumerService.hpp"
#include "productionline/worker/WorkerConfig.hpp"

#include <string>
#include <vector>
#include <map>
#include <log4cplus/logger.h>

namespace test {
namespace record {

// 类型别名：使用新架构的 ConsumeResult
using TestResult = consumer::ConsumeResult;

/**
 * @brief 录制测试参数
 */
struct RecordTestParams {
    std::string format;         ///< 输出格式 (mp4, mkv, mov, ts, flv, avi)
    double duration;            ///< 录制时长（秒）
    
    RecordTestParams(
        const std::string& fmt = "mp4",
        double dur = 10.0
    ) : format(fmt), duration(dur) {}
};

/**
 * @brief 录制测试套件
 * 
 * 实现 ITestModule 接口，提供完整的录制测试功能。
 * 
 * 架构设计：
 * - 所有录制测试使用 ExecuteMode::SINGLE
 * - 消费标志：CONSUME_SAVE_ENCODED
 */
class RecordTestSuite : public common::ITestModule {
public:
    RecordTestSuite() = default;
    ~RecordTestSuite() override = default;
    
    // ========================================
    // ITestModule 接口实现
    // ========================================
    
    std::string getName() const override { return "record"; }
    std::string getDescription() const override { return "流录制测试"; }
    
    int run(int argc, char* argv[]) override;
    void printHelp() const override;
    void listTests() const override;
    std::vector<std::string> getTestNames() const override;
    
    // ========================================
    // 核心测试方法（与 ExecuteMode 对齐）
    // ========================================
    
    /**
     * @brief 单路消费测试（ExecuteMode::SINGLE + CONSUME_SAVE_ENCODED）
     * 
     * 录制测试统一入口
     * 
     * @param input_source 输入源（RTSP URL 或文件路径）
     * @param output_path 输出文件路径
     * @param params 录制参数
     * @return 测试结果
     */
    static TestResult runSingle(
        const std::string& input_source,
        const std::string& output_path,
        const RecordTestParams& params = RecordTestParams()
    );
    
    /**
     * @brief 获取预定义测试参数
     */
    static const std::map<std::string, RecordTestParams>& getPredefinedTests();

private:
    /**
     * @brief 解析命令行参数
     */
    bool parseArgs(int argc, char* argv[], WorkerConfig& config, std::string& output_path, RecordTestParams& params);
    
    /**
     * @brief 获取模块级日志实例
     */
    static log4cplus::Logger& getLogger();
};

} // namespace record
} // namespace test

#endif // RECORD_TEST_SUITE_HPP
