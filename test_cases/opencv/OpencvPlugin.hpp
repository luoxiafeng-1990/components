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
#include "consumptionline/core/BufferConsumerService.hpp"
#include "consumptionline/config/ConsumerTypeConfig.hpp"
#include "productionline/worker/config/MultiWorkerConfig.hpp"
#include "productionline/worker/config/WorkerConfigs.hpp"
#include "productionline/line/WorkerSyncCoordinator.hpp"

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <optional>

namespace test {
namespace opencv {

using TestResult = consumer::ConsumeResult;

struct OpencvTestParams {
    using OpType = WorkerConfig::ConsumerTypeConfig::OpencvType::OpType;
    OpType opencv_op  = OpType::NONE;
    bool   use_hardware = true;
    std::string params_str;

    // resize 专用参数（不走 --params）
    bool has_resize_size = false;
    int resize_width = 0;
    int resize_height = 0;

    bool has_resize_scale = false;
    double resize_fx = 0.0;
    double resize_fy = 0.0;

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
    std::string dst_fmt_;
    int dst_width_ = 0;
    int dst_height_ = 0;
    int src_width_ = 0;
    int src_height_ = 0;
    double crop_x_ = 0.0;
    double crop_y_ = 0.0;
    double resize_fx_ = 0.0;
    double resize_fy_ = 0.0;
    int interpolation = 0;
    int max_frames_ = 5;
    int min_fps_ = 0;
    bool use_software = false;
    bool jpeg_progressive_ = false;
    int quality_ = 95;
    bool enable_pix_compare = false;
    bool enable_api_exception = false;
    bool enable_perf = false;
    bool verbose_ = false;
    bool show_list_ = false;
    std::vector<std::string> positional_args_;
    OpencvTestParams params_;
    std::string src_pix_fmt;

    bool parseArgs(int argc, char* argv[], WorkerConfig& config, OpencvTestParams& params);
    int runPredefinedTest(const std::string& test_name, const std::string& path);
};

}
}

#endif // OPENCV_PLUGIN_HPP

