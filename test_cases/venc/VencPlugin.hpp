/**
 * @file VencPlugin.hpp
 * @brief 视频编码测试插件（与 VdecPlugin / IOptionPlugin 架构对齐）
 *
 * PSNR/SSIM：-p/-S 写入 consumer_type.compare，单路编码时在 test_module_main 中
 * 走 runEncodeQualityCompare（源 YUV vs 编码→软解），与解码双路 COMPARE 区分。
 *
 * 显示：与 vdec 一致，在命令行追加子命令 display（DisplayPlugin）启用上屏；
 * test_module_main 在 encode + display.enable 时走 runEncodeDecodeDisplay（编码→软解→消费解码池）。
 */

#ifndef VENC_PLUGIN_HPP
#define VENC_PLUGIN_HPP

#include "../common/IOptionPlugin.hpp"
#include "../common/ExecuteMode.hpp"
#include "../common/DataSourceOptions.hpp"
#include "consumptionline/core/BufferConsumerService.hpp"
#include "productionline/worker/config/MultiWorkerConfig.hpp"
#include "productionline/worker/config/WorkerConfigs.hpp"

#include <map>
#include <string>
#include <vector>

namespace test {
namespace venc {

struct EncodeTestParams {
    std::string codec;
    std::string profile;
    int bitrate = 0;
    int gop_size = 30;
    bool use_hardware = true;
    int rc_mode = 1;
    /** CQP 量化参数（rc_mode=2），默认 28 */
    int cqp_qp = 28;
    std::string input_format = "nv12";
    int input_width = 1920;
    int input_height = 1080;
    double input_fps = 30.0;
    double output_fps = 0.0;
    int output_width = 0;
    int output_height = 0;
    int jpeg_quality = 80;
    std::string predefined_name;

    EncodeTestParams() = default;
    EncodeTestParams(std::string c, std::string p, int br, int gop, bool hw)
        : codec(std::move(c))
        , profile(std::move(p))
        , bitrate(br)
        , gop_size(gop)
        , use_hardware(hw) {}

    bool isPredefined() const { return !predefined_name.empty(); }
};

class VencPlugin : public IOptionPlugin {
public:
    std::string getName() const override { return "venc"; }
    std::string getDescription() const override { return "视频编码测试 (YUV→H.264/H.265/JPEG)"; }

    void registerOptions(CLI::App& app) override;
    void applyTo(WorkerConfig& config) const override;
    void listTests() const override;
    int handlePreActions() override;
    std::vector<WorkerConfig> buildPipelineConfigs(const WorkerConfig& shared_config) override;
    std::string getTestName() const override;

    static const std::map<std::string, EncodeTestParams>& getPredefinedTests();
    /// 使用当前插件解析到的 input_path_ 构建编码配置（非 static，因依赖实例状态）
    WorkerConfig buildEncodeConfig(const EncodeTestParams& params);

private:
    EncodeTestParams resolveParams() const;

    bool show_list_ = false;
    EncodeTestParams params_;
    std::string input_path_;
    std::vector<std::string> positional_args_;

    std::string encoded_output_path_;
    int max_frames_ = -1;
    int loop_count_ = 1;  ///< 输入文件循环遍数（默认 1）
    bool verbose_ = false;
    int threads_ = 0;

    bool enable_psnr_ = false;
    bool enable_ssim_ = false;
    double min_psnr_ = 30.0;
    double min_ssim_ = 0.95;

    /// DataSource 横切选项（venc 用 --buffer-count，避免与 -b bitrate 冲突）
    DataSourceOptions ds_opts_;
    /// ENC 默认 BufferPool 槽位数（用户要求默认 16；CLI --buffer-count 可覆盖）
    static constexpr int kDefaultEncodeBufferCount = 16;
};

// ============================================================
// 兼容 test_module_main 的编码质量验证/编码+解码+显示入口
// ============================================================
consumer::ConsumeResult runEncodeQualityCompare(
    const WorkerConfig& encode_cfg,
    const WorkerConfig& shared_cfg,
    const std::string& test_name);

consumer::ConsumeResult runEncodeDecodeDisplay(
    const WorkerConfig& encode_cfg,
    const WorkerConfig& shared_cfg,
    const std::string& test_name);

} // namespace venc
} // namespace test

#endif
