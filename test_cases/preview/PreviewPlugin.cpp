/**
 * @file PreviewPlugin.cpp
 * @brief JPEG 预览插件实现
 */

#include "PreviewPlugin.hpp"
#include "../common/third_party/CLI11.hpp"
#include "consumptionline/config/ConsumerTypeConfigBuilder.hpp"

namespace test {
namespace preview {

std::string PreviewPlugin::getName() const {
    return "preview";
}

std::string PreviewPlugin::getDescription() const {
    return "JPEG 预览输出到命名管道（WebUI 实时预览）";
}

void PreviewPlugin::registerOptions(CLI::App& app) {
    app.add_option("--pipe", output_pipe_, "输出 FIFO 路径（同进程模式可省略）");
    app.add_option("--quality", quality_, "JPEG 质量 (默认: 80)")
        ->check(CLI::Range(1, 100));
    app.add_option("--fps", target_fps_, "预览帧率 (默认: 15)")
        ->check(CLI::Range(1, 60));
    app.add_option("--encoder", encoder_name_,
                   "编码器 (jpeg_taco|mjpeg, 默认: jpeg_taco)");
}

void PreviewPlugin::applyTo(WorkerConfig& config) const {
    config.consumer_type = ConsumerTypeConfigBuilder(config.consumer_type)
        .setJpegEncodeConfig(JpegEncodeConfigBuilder()
            .setEnable(true)
            .setOutputPipe(output_pipe_)
            .setQuality(quality_)
            .setTargetFps(target_fps_)
            .setEncoderName(encoder_name_)
            .build())
        .build();
}

} // namespace preview
} // namespace test
