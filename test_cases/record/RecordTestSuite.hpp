/**
 * @file RecordTestSuite.hpp
 * @brief 录制测试套件
 * 
 * 封装所有录制相关的测试功能，包括：
 * - RTSP 流录制
 * - 文件重封装
 * - 多种容器格式测试（MP4/MKV/MOV/TS/FLV/AVI）
 * 
 * 使用示例：
 * @code
 * ./qa_cases record --rtsp rtsp://192.168.1.100/stream --output /tmp/out.mp4
 * ./qa_cases record --file input.mkv --output /tmp/out.mp4
 * ./qa_cases record -h
 * @endcode
 * 
 * @version 3.1
 */

#ifndef RECORD_TEST_SUITE_HPP
#define RECORD_TEST_SUITE_HPP

#include "../common/ITestModule.hpp"
#include "../common/TestExecutor.hpp"
#include "productionline/worker/WorkerConfig.hpp"

#include <string>
#include <vector>
#include <map>
#include <log4cplus/logger.h>

namespace test {
namespace record {

/**
 * @brief 录制测试参数
 */
struct RecordTestParams {
    std::string format;         ///< 容器格式 (mp4, mkv, mov, ts, flv, avi, 3gp)
    double duration;            ///< 录制时长（秒，-1=无限制）
    int64_t max_size_mb;        ///< 最大文件大小（MB，-1=无限制）
    
    RecordTestParams(
        const std::string& fmt = "mp4",
        double dur = 10.0,
        int64_t max_mb = -1
    ) : format(fmt), duration(dur), max_size_mb(max_mb) {}
};

/**
 * @brief 录制测试套件
 * 
 * 实现 ITestModule 接口，提供完整的录制测试功能。
 */
class RecordTestSuite : public common::ITestModule {
public:
    RecordTestSuite() = default;
    ~RecordTestSuite() override = default;
    
    // ========================================
    // ITestModule 接口实现
    // ========================================
    
    std::string getName() const override { return "record"; }
    std::string getDescription() const override { return "录制测试"; }
    
    int run(int argc, char* argv[]) override;
    void printHelp() const override;
    void listTests() const override;
    std::vector<std::string> getTestNames() const override;
    
    // ========================================
    // 核心测试方法
    // ========================================
    
    /**
     * @brief 执行 RTSP 录制测试
     */
    static common::TestResult runRtspRecord(
        const std::string& rtsp_url,
        const std::string& output_path,
        const RecordTestParams& params = RecordTestParams()
    );
    
    /**
     * @brief 执行文件重封装测试
     */
    static common::TestResult runFileRecord(
        const std::string& input_path,
        const std::string& output_path,
        const RecordTestParams& params = RecordTestParams()
    );
    
    /**
     * @brief 执行多格式录制测试
     */
    static common::TestResult runAllFormatsRecord(
        const std::string& input_source,
        const std::string& output_dir
    );
    
    /**
     * @brief 获取预定义测试参数
     */
    static const std::map<std::string, RecordTestParams>& getPredefinedTests();

private:
    bool parseArgs(int argc, char* argv[], WorkerConfig& config, 
                   std::string& output_path, RecordTestParams& params);
    
    /**
     * @brief 获取模块级日志实例
     */
    static log4cplus::Logger& getLogger();
};

} // namespace record
} // namespace test

#endif // RECORD_TEST_SUITE_HPP
