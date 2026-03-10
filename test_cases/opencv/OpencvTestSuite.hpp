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
    using OpType = WorkerConfig::ConsumerTypeConfig::OpencvType::OpType;
    OpType opencv_op  = OpType::NONE;  ///< 操作类型（NONE/RESIZE/CROP）
    bool   use_hardware = true;        ///< 是否使用硬件解码
    std::string params_str;            ///< 编码参数，格式: "<op>+<output_w>+<output_h>"

    bool hasOpencvOp() const { return opencv_op != OpType::NONE; }
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
