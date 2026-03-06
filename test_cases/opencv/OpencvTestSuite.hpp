#ifndef TAOPENCV_TEST_SUITE_HPP
#define TAOPENCV_TEST_SUITE_HPP

#include "../common/ITestModule.hpp"
#include "productionline/io/BufferConsumerService.hpp"
#include "productionline/worker/WorkerConfig.hpp"

#include <string>
#include <vector>
#include <map>
#include <log4cplus/logger.h>

namespace test {
namespace opencv {

// 类型别名：使用新架构的 ConsumeResult
using TestResult = consumer::ConsumeResult;

struct OpencvTestParams {
    std::string codec;           ///< 编解码器 (h264, h265, mjpeg)
    int width;                   ///< 分辨率宽度
    int height;                  ///< 分辨率高度
    double fps;                  ///< 目标帧率
    bool use_hardware;           ///< 是否使用硬件解码（默认 true）
    std::string predefined_name; ///< 匹配的预定义测试名称（空表示未使用预定义测试）
    
    OpencvTestParams(
        const std::string& c = "h264",
        int w = 1920,
        int h = 1080,
        double f = 30.0,
        bool hw = true
    ) : codec(c), width(w), height(h), fps(f), use_hardware(hw) {}
    
    /// 是否使用了预定义测试
    bool isPredefined() const { return !predefined_name.empty(); }
};

class OpencvTestSuite : public common::ITestModule {
public:
    OpencvTestSuite() = default;
    ~OpencvTestSuite() override = default;
    
    // ========================================
    // ITestModule 接口实现
    // ========================================
    
    std::string getName() const override { return "opencv"; }
    std::string getDescription() const override { return "opencv测试"; }
    
    int run(int argc, char* argv[]) override;
    void printHelp() const override;
    void listTests() const override;
    std::vector<std::string> getTestNames() const override;
    
    static const std::map<std::string, OpencvTestParams>& getPredefinedTests();
    static uint32_t buildConsumeFlags(const WorkerConfig& config);

private:
    bool parseArgs(int argc, char* argv[], WorkerConfig& config, OpencvTestParams& params);
    int runPredefinedTest(const std::string& test_name, const std::string& path);
    
};

}
}

#endif
