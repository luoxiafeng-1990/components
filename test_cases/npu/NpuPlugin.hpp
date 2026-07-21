/**
 * @file NpuPlugin.hpp
 * @brief NPU 推理插件
 *
 * 作为独立子命令 npu 注册，管理 NPU 推理相关的命令行参数解析和配置注入。
 * 在 entries 中位于 display 之后注册，applyTo 自然在 display 之后调用，
 * 可自动检测 NPU+Display 联动。
 *
 * 用法：
 * @code
 * ./qa_cases vdec --file video.mp4 npu --algorithm yolov8_det --model model.nb
 * @endcode
 */

#ifndef TEST_NPU_PLUGIN_HPP
#define TEST_NPU_PLUGIN_HPP

#include "../common/IOptionPlugin.hpp"

#include <string>

namespace CLI { class App; }

namespace test {
namespace npu {

class NpuPlugin : public IOptionPlugin {
public:
    std::string getName() const override;
    std::string getDescription() const override;

    void registerOptions(CLI::App& app) override;
    int handlePreActions() override;
    void applyTo(WorkerConfig& config) const override;

private:
    std::string model_path_;
    std::string algorithm_;
    float       conf_threshold_     = 0.25f;
    float       nms_threshold_      = 0.45f;
    int         npu_core_index_     = 0;
    bool        use_physical_addr_  = false;
    bool        enable_draw_        = false;
    int         inference_interval_ = 1;
};

} // namespace npu
} // namespace test

#endif // TEST_NPU_PLUGIN_HPP
