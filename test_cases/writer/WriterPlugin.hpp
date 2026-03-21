/**
 * @file WriterPlugin.hpp
 * @brief 帧写入测试套件
 * 
 * 封装所有帧写入相关的测试功能，包括：
 * - YUV 格式写入测试
 * - RGB 格式写入测试
 * - 批量格式测试
 * 
 * 架构设计：
 * - 实现 IOptionPlugin 接口，作为主插件（canExecute = true）
 * - 使用 ExecuteMode 静态工具类执行 SINGLE 模式
 * - 帧写入测试使用 ExecuteMode::SINGLE + CONSUME_SAVE_RAW
 * 
 * 使用示例：
 * @code
 * ./qa_cases writer --format nv12 --input video.mp4 --output /tmp/out.yuv
 * ./qa_cases writer --format argb888 --input video.mp4
 * ./qa_cases writer -h
 * @endcode
 * 
 * @version 6.0 - 迁移到 CLI11
 */

#ifndef WRITER_PLUGIN_HPP
#define WRITER_PLUGIN_HPP

#include "../common/IOptionPlugin.hpp"
#include "../common/ExecuteMode.hpp"
#include "consumptionline/BufferConsumerService.hpp"
#include "productionline/worker/WorkerConfig.hpp"

#include <map>
#include <string>
#include <vector>

namespace CLI { class App; }

namespace test {
namespace writer {

// 类型别名：使用新架构的 ConsumeResult
using TestResult = consumer::ConsumeResult;

/**
 * @brief Writer 测试参数
 */
struct WriterTestParams {
    OutputFormat format;    ///< 输出格式
    std::string description;///< 格式描述
    int width;              ///< 输出宽度
    int height;             ///< 输出高度
    int save_frames;        ///< 保存帧数
    bool use_hardware;      ///< 是否使用硬件解码（默认 true）
    
    WriterTestParams(
        OutputFormat fmt = OutputFormat::YUV_NV12,
        const std::string& desc = "NV12",
        int w = 1920, int h = 1080,
        int frames = 10,
        bool hw = true
    ) : format(fmt), description(desc), width(w), height(h), save_frames(frames), use_hardware(hw) {}
};

/**
 * @brief 帧写入测试套件
 * 
 * 实现 IOptionPlugin 接口，作为主插件提供完整的帧写入测试功能。
 * 
 * 架构设计：
 * - 所有帧写入测试使用 ExecuteMode::SINGLE
 * - 消费标志：CONSUME_COUNT | CONSUME_SAVE_RAW
 */
class WriterPlugin : public IOptionPlugin {
public:
    WriterPlugin() = default;
    ~WriterPlugin() override = default;
    
    // ========================================
    // IOptionPlugin 接口实现
    // ========================================
    
    std::string getName() const override { return "writer"; }
    std::string getDescription() const override { return "帧写入测试"; }
    
    void registerOptions(CLI::App& app) override;
    void applyTo(WorkerConfig& config) const override;
    void listTests() const override;
    
    int handlePreActions() override;
    std::vector<WorkerConfig> buildPipelineConfigs(const WorkerConfig& shared_config) override;
    std::string getTestName() const override;
    
    /**
     * @brief 获取预定义测试参数
     */
    static const std::map<std::string, WriterTestParams>& getPredefinedTests();
    
    /**
     * @brief 获取所有 RGB 格式列表
     */
    static const std::vector<std::pair<OutputFormat, std::string>>& getRgbFormats();
    
    /**
     * @brief 获取所有 YUV 格式列表
     */
    static const std::vector<std::pair<OutputFormat, std::string>>& getYuvFormats();

private:
    // ========================================
    // 解析状态（由 CLI11 自动填充）
    // ========================================
    bool show_list_ = false;
    std::string input_path_;
    std::string output_path_;
    std::string decoder_str_;
    std::string format_str_ = "nv12";
    std::vector<std::string> positional_args_;
    WriterTestParams params_;
    bool verbose_ = false;
    bool format_specified_ = false;
    bool all_rgb_ = false;
    bool all_yuv_ = false;
};

} // namespace writer
} // namespace test

#endif // WRITER_PLUGIN_HPP
