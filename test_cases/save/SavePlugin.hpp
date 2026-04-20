/**
 * @file SavePlugin.hpp
 * @brief 保存测试套件（合并原 RecordPlugin + WriterPlugin）
 * 
 * 统一处理两种保存模式：
 * - Stream（流录制）：编码 packet -> 容器文件 (mp4/mkv/ts/...)
 * - Frame（帧导出）：解码帧 -> 原始像素文件 (yuv/rgb)
 * 
 * 模式根据 --format 值或其他参数自动推断，容器格式 (mp4/mkv/...)
 * 和像素格式 (nv12/rgb888/...) 天然不重叠，无需手动指定模式。
 *
 * 使用示例：
 * @code
 * // Stream 模式
 * ./qa_cases save -r rtsp://192.168.1.100/stream -o /tmp/out.mp4
 * ./qa_cases save -i input.mkv -f mkv -o /tmp/remux.mkv
 *
 * // Frame 模式
 * ./qa_cases save -i video.mp4 -f rgb888 -o output.rgb
 * ./qa_cases save -i video.mp4 --all-rgb -o /tmp/rgb_test
 * @endcode
 */

#ifndef SAVE_PLUGIN_HPP
#define SAVE_PLUGIN_HPP

#include "../common/IOptionPlugin.hpp"
#include "../common/ExecuteMode.hpp"
#include "../common/DataSourceOptions.hpp"
#include "consumptionline/core/BufferConsumerService.hpp"
#include "productionline/worker/config/MultiWorkerConfig.hpp"
#include "productionline/worker/config/WorkerConfigs.hpp"

#include <map>
#include <string>
#include <vector>

namespace CLI { class App; class Option; }

namespace test {
namespace save {

using TestResult = consumer::ConsumeResult;

enum class SaveMode {
    STREAM,  ///< 保存编码流 -> 容器文件 (mp4/mkv/ts/...)
    FRAME    ///< 保存解码帧 -> 原始像素文件 (yuv/rgb)
};

class SavePlugin : public IOptionPlugin {
public:
    SavePlugin() = default;
    ~SavePlugin() override = default;

    std::string getName() const override { return "save"; }
    std::string getDescription() const override { return "保存测试（流录制 / 帧导出）"; }

    void registerOptions(CLI::App& app) override;
    void applyTo(WorkerConfig& config) const override;
    void listTests() const override;
    int handlePreActions() override;
    std::vector<WorkerConfig> buildPipelineConfigs(const WorkerConfig& shared_config) override;
    std::string getTestName() const override;

    static bool isContainerFormat(const std::string& fmt);
    static const std::vector<std::string>& getContainerFormats();
    static const std::vector<std::pair<OutputFormat, std::string>>& getRgbFormats();
    static const std::vector<std::pair<OutputFormat, std::string>>& getYuvFormats();

private:
    struct StreamTestParams {
        std::string format;
        double duration;
    };

    struct FrameTestParams {
        OutputFormat format;
        std::string description;
        int width = 1920;
        int height = 1080;
        int save_frames = 10;
        bool use_hardware = true;
    };

    static const std::map<std::string, StreamTestParams>& getStreamTests();
    static const std::map<std::string, FrameTestParams>& getFrameTests();

    std::vector<WorkerConfig> buildStreamPipeline(const WorkerConfig& shared_config);
    std::vector<WorkerConfig> buildFramePipeline(const WorkerConfig& shared_config);

    // CLI state
    bool show_list_ = false;
    std::string input_path_;
    std::string output_path_;
    std::string format_str_;
    bool verbose_ = false;
    std::vector<std::string> positional_args_;

    CLI::Option* rtsp_opt_ = nullptr;
    CLI::Option* duration_opt_ = nullptr;
    CLI::Option* decoder_opt_ = nullptr;
    CLI::Option* frames_opt_ = nullptr;

    SaveMode resolved_mode_ = SaveMode::FRAME;

    // Stream-specific
    std::string container_format_ = "mp4";
    double duration_ = 10.0;
    bool all_container_formats_ = false;

    // Frame-specific
    std::string decoder_str_;
    int save_frames_ = 10;
    DataSourceOptions ds_opts_;    ///< DataSource 横切选项
    bool all_rgb_ = false;
    bool all_yuv_ = false;
    bool format_specified_ = false;
    OutputFormat pixel_format_ = OutputFormat::YUV_NV12;
    std::string pixel_desc_ = "NV12";
    int width_ = 1920;
    int height_ = 1080;
    bool use_hardware_ = true;
};

} // namespace save
} // namespace test

#endif // SAVE_PLUGIN_HPP
