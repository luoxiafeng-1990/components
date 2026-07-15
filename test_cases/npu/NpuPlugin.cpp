/**
 * @file NpuPlugin.cpp
 * @brief NPU 推理插件实现
 */

#include "NpuPlugin.hpp"
#include "../common/third_party/CLI11.hpp"
#include "consumptionline/config/ConsumerTypeConfigBuilder.hpp"
#include "consumptionline/types/npu/NpuAlgorithm.hpp"

#include <iostream>
#include <unistd.h>

namespace test {
namespace npu {

std::string NpuPlugin::getName() const {
    return "npu";
}

std::string NpuPlugin::getDescription() const {
    return "NPU 推理";
}

void NpuPlugin::registerOptions(CLI::App& app) {
    app.add_option("--model,--model-path", model_path_, ".nb 模型文件路径")->required();
    app.add_option("--algorithm", algorithm_,
                   "检测算法: yolov5_det|yolov8_det|yolo11_det|yolov12_det")
        ->check(CLI::IsMember(consumer::supportedNpuAlgorithmNames()))
        ->required();
    app.add_option("--conf-threshold", conf_threshold_, "置信度阈值 (默认: 0.25)");
    app.add_option("--nms-threshold", nms_threshold_, "NMS IoU 阈值 (默认: 0.45)");
    app.add_option("--npu-core", npu_core_index_, "NPU 核心索引 (默认: 0)");
    app.add_flag("--physical-addr", use_physical_addr_, "使用物理地址零拷贝输入");
    app.add_flag("--draw-detections", enable_draw_, "推理后在画面上绘制检测框");
    app.add_option("--inference-interval", inference_interval_, "每 N 帧执行一次推理 (默认: 1)");
}

int NpuPlugin::handlePreActions() {
    if (model_path_.empty()) {
        std::cerr << "NpuPlugin: model path is empty\n";
        return 1;
    }
    if (access(model_path_.c_str(), R_OK) != 0) {
        std::cerr << "NpuPlugin: model not found '" << model_path_ << "'\n";
        return 1;
    }
    return -1;
}

void NpuPlugin::applyTo(WorkerConfig& config) const {
    auto npu_builder = NpuInferenceConfigBuilder(config.consumer_type.npu_inference)
        .setEnable(true)
        .setModelPath(model_path_)
        .setAlgorithm(consumer::parseNpuAlgorithm(algorithm_))
        .setConfThreshold(conf_threshold_)
        .setNmsThreshold(nms_threshold_)
        .setNpuCoreIndex(npu_core_index_)
        .setUsePhysicalAddr(use_physical_addr_)
        .setEnableDraw(enable_draw_)
        .setInferenceInterval(inference_interval_);

    if (config.consumer_type.display.enable) {
        npu_builder.setEnableDraw(true);
        if (inference_interval_ <= 1)
            npu_builder.setInferenceInterval(15);
    }

    config.consumer_type = ConsumerTypeConfigBuilder(config.consumer_type)
        .setNpuInferenceConfig(npu_builder.build())
        .build();
}

} // namespace npu
} // namespace test
