/**
 * @file PreviewPlugin.hpp
 * @brief JPEG 预览插件
 *
 * 作为独立子命令 preview 注册，管理 JPEG 编码预览相关参数。
 * 将解码帧编码为 JPEG 并通过命名管道输出，供 WebUI 实时预览。
 *
 * 用法：
 * @code
 * ./qa_cases vdec --file video.mp4 preview --pipe /tmp/preview.fifo
 * ./qa_cases vdec --file video.mp4 npu --model m.nb preview --pipe /tmp/p.fifo display --vendor taco
 * @endcode
 *
 * @version 3.3
 */

#ifndef TEST_PREVIEW_PLUGIN_HPP
#define TEST_PREVIEW_PLUGIN_HPP

#include "../common/IOptionPlugin.hpp"

#include <string>

namespace CLI { class App; }

namespace test {
namespace preview {

class PreviewPlugin : public IOptionPlugin {
public:
    std::string getName() const override;
    std::string getDescription() const override;

    void registerOptions(CLI::App& app) override;
    void applyCliToConfig(WorkerConfig& config) const override;

private:
    std::string output_pipe_;
    int         quality_      = 80;
    int         target_fps_   = 15;
    std::string encoder_name_ = "jpeg_taco";
};

} // namespace preview
} // namespace test

#endif // TEST_PREVIEW_PLUGIN_HPP
