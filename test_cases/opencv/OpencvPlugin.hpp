/**
 * @file OpencvPlugin.hpp
 * @brief OpenCV test plugin
 *
 * Implements IOptionPlugin interface for OpenCV arithmetic/logical operations testing.
 * Supports operations: ADD, ABSDIFF, ADD_WEIGHTED, BITWISE_AND/OR/XOR/NOT, SAVE_LOAD_IMG
 */

#ifndef OPENCV_PLUGIN_HPP
#define OPENCV_PLUGIN_HPP

#include "../common/IOptionPlugin.hpp"
#include "../common/ExecuteMode.hpp"
#include "../common/DataSourceOptions.hpp"
#include "consumptionline/BufferConsumerService.hpp"
#include "productionline/worker/WorkerConfig.hpp"

#include <string>
#include <vector>
#include <map>

namespace test {
namespace opencv {

using TestResult = consumer::ConsumeResult;

struct OpencvTestParams {
    using OpType = WorkerConfig::ConsumerTypeConfig::OpencvType::OpType;
    OpType opencv_op  = OpType::NONE;
    bool   use_hardware = true;
    std::string params_str;

    bool hasOpencvOp() const { return opencv_op != OpType::NONE; }
};

class OpencvPlugin : public IOptionPlugin {
public:
    OpencvPlugin() = default;
    ~OpencvPlugin() override = default;

    std::string getName() const override { return "opencv"; }
    std::string getDescription() const override { return "OpenCV operations test"; }

    void registerOptions(CLI::App& app) override;
    void applyTo(WorkerConfig& config) const override;
    void listTests() const override;

    int handlePreActions() override;
    std::vector<WorkerConfig> buildPipelineConfigs(const WorkerConfig& shared_config) override;
    std::string getTestName() const override;

    static const std::map<std::string, OpencvTestParams>& getPredefinedTests();
    static uint32_t buildConsumeFlags(const WorkerConfig& config);

    // Legacy methods from old ITestModule interface (for backward compatibility)
    int run(int argc, char* argv[]);
    void printHelp() const;
    std::vector<std::string> getTestNames() const;

private:
    std::string test_name_;
    std::string file_path_;
    OpencvTestParams params_;
    DataSourceOptions data_source_opts_;

    bool parseArgs(int argc, char* argv[], WorkerConfig& config, OpencvTestParams& params);
    int runPredefinedTest(const std::string& test_name, const std::string& path);
};

}
}

#endif // OPENCV_PLUGIN_HPP

