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
#include "consumptionline/config/ConsumerTypeConfig.hpp"
#include "productionline/worker/config/MultiWorkerConfig.hpp"
#include "productionline/worker/config/WorkerConfigs.hpp"
#include "productionline/line/WorkerSyncCoordinator.hpp"

#include <string>
#include <vector>
#include <map>
#include <memory>

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

private:
    std::string input_path_;
    std::string case_str_;
    std::string params_str_;
    int max_frames_ = -1;
    bool use_hardware_ = true;
    bool enable_psnr_ = false;
    bool enable_ssim_ = false;
    bool verbose_ = false;
    bool show_list_ = false;
    std::vector<std::string> positional_args_;
    OpencvTestParams params_;

    bool parseArgs(int argc, char* argv[], WorkerConfig& config, OpencvTestParams& params);
    int runPredefinedTest(const std::string& test_name, const std::string& path);
};

}
}

#endif // OPENCV_PLUGIN_HPP

