/**
 * @file PPPlugin.hpp
 * @brief 后处理（PP）测试套件
 * 
 * 封装所有后处理相关的测试功能，包括：
 * - PP0（通道0）YUV 格式测试
 * - PP1（通道1）RGB/YUV 格式测试
 * - Multi-PP（双通道）测试
 * - 裁剪和缩放测试
 * - PSNR/SSIM 质量验证
 * 
 * 架构设计：
 * - 实现 IOptionPlugin 接口，作为主插件（canExecute = true）
 * - 使用 ExecuteMode 静态工具类执行 SINGLE / COMPARE / PARALLEL 模式
 * 
 * 使用示例：
 * @code
 * ./qa_cases pp --format nv12 --channel 0 --input video.mp4
 * ./qa_cases pp --format argb888 --channel 1 --input video.mp4
 * ./qa_cases pp --psnr video.mp4              # HW vs SW 比较
 * ./qa_cases pp --channel 0,1 --psnr video.mp4  # 通道比较 (v2.27)
 * ./qa_cases pp -h
 * @endcode
 * 
 * @version 5.0 - 重构为 IOptionPlugin 插件架构
 */

#ifndef PP_PLUGIN_HPP
#define PP_PLUGIN_HPP

#include "../common/IOptionPlugin.hpp"
#include "../common/ExecuteMode.hpp"
#include "../common/DataSourceOptions.hpp"
#include "consumptionline/BufferConsumerService.hpp"
#include "productionline/worker/WorkerConfig.hpp"

#include <string>
#include <vector>
#include <sstream>
#include <map>

namespace test {
namespace pp {

// 类型别名：使用新架构的 ConsumeResult
using TestResult = consumer::ConsumeResult;

/**
 * @brief PP 测试参数（可扩展多通道设计）
 * 
 * 设计原则：
 * - channels: 启用的通道列表，如 [0], [1], [0,1], [0,1,2]
 * - formats: 每个通道的输出格式，与 channels 一一对应
 * - v2.27: 通道使用数字格式（如 "0", "1", "0,1"）
 */
struct PPTestParams {
    std::vector<int> channels;           ///< 启用的通道列表
    std::vector<OutputFormat> formats;   ///< 每个通道的输出格式
    int width;                           ///< 输出宽度
    int height;                          ///< 输出高度
    ColorStandard color_std;             ///< 颜色标准
    
    // 裁剪参数（可选）
    int crop_x;
    int crop_y;
    int crop_w;
    int crop_h;
    
    // 解码方式
    bool use_hardware;                   ///< 是否使用硬件解码（默认 true）
    
    // ========================================
    // 辅助方法
    // ========================================
    
    /// 获取 channel 字符串（用于日志）
    std::string getChannelString() const {
        if (channels.empty()) return "0";
        std::string result;
        for (size_t i = 0; i < channels.size(); ++i) {
            if (i > 0) result += ",";
            result += std::to_string(channels[i]);
        }
        return result;
    }
    
    /// 获取指定通道的格式（如果 formats 不够，使用最后一个或默认值）
    OutputFormat getFormat(size_t index) const {
        if (formats.empty()) return OutputFormat::YUV_NV12;
        if (index < formats.size()) return formats[index];
        return formats.back();  // 不够时使用最后一个
    }
    
    /// 是否为多通道模式
    bool isMultiChannel() const { return channels.size() > 1; }
    
    // ========================================
    // 构造函数（保持兼容性）
    // ========================================
    
    // 默认构造 - channels 为空表示未使用预定义测试
    PPTestParams()
        : channels(), formats({OutputFormat::YUV_NV12}),
          width(1920), height(1080), color_std(ColorStandard::BT601),
          crop_x(0), crop_y(0), crop_w(0), crop_h(0), use_hardware(true) {}
    
    // 单通道构造（4参数）- 兼容 {"pp0", NV12, 1920, 1080}
    PPTestParams(
        const std::string& ch,
        OutputFormat fmt,
        int w, int h
    ) : channels(parseChannelString(ch)), formats({fmt}),
        width(w), height(h), color_std(ColorStandard::BT601),
        crop_x(0), crop_y(0), crop_w(0), crop_h(0), use_hardware(true) {}
    
    // 单通道构造（5参数，带 ColorStandard）
    PPTestParams(
        const std::string& ch,
        OutputFormat fmt,
        int w, int h,
        ColorStandard std
    ) : channels(parseChannelString(ch)), formats({fmt}),
        width(w), height(h), color_std(std),
        crop_x(0), crop_y(0), crop_w(0), crop_h(0), use_hardware(true) {}
    
    // 单通道构造（带裁剪参数，9参数）
    PPTestParams(
        const std::string& ch,
        OutputFormat fmt,
        int w, int h,
        ColorStandard std,
        int cx, int cy, int cw, int ch_
    ) : channels(parseChannelString(ch)), formats({fmt}),
        width(w), height(h), color_std(std),
        crop_x(cx), crop_y(cy), crop_w(cw), crop_h(ch_), use_hardware(true) {}
    
    // Multi-PP 构造（4参数）- 兼容 {NV12, RGB888, 1920, 1080}
    PPTestParams(
        OutputFormat pp0_fmt,
        OutputFormat pp1_fmt,
        int w, int h
    ) : channels({0, 1}), formats({pp0_fmt, pp1_fmt}),
        width(w), height(h), color_std(ColorStandard::BT601),
        crop_x(0), crop_y(0), crop_w(0), crop_h(0), use_hardware(true) {}
    
    // Multi-PP 构造（5参数，带 ColorStandard）
    PPTestParams(
        OutputFormat pp0_fmt,
        OutputFormat pp1_fmt,
        int w, int h,
        ColorStandard std
    ) : channels({0, 1}), formats({pp0_fmt, pp1_fmt}),
        width(w), height(h), color_std(std),
        crop_x(0), crop_y(0), crop_w(0), crop_h(0), use_hardware(true) {}
    
private:
    /// 解析通道字符串（"0" → [0], "1" → [1], "0,1" → [0,1]）
    /// v2.27: 移除旧格式 "pp0", "pp1", "multi"
    static std::vector<int> parseChannelString(const std::string& ch) {
        std::vector<int> result;
        std::stringstream ss(ch);
        std::string item;
        while (std::getline(ss, item, ',')) {
            try {
                result.push_back(std::stoi(item));
            } catch (...) {}
        }
        return result.empty() ? std::vector<int>{0} : result;
    }
};

/**
 * @brief 后处理测试套件
 * 
 * 实现 IOptionPlugin 接口，作为主插件提供完整的后处理测试功能。
 * 
 * 架构设计：
 * - 所有测试方法通过 ExecuteMode 静态工具类执行
 * - ExecuteMode::single()   → SINGLE
 * - ExecuteMode::compare()  → COMPARE (PSNR/SSIM)
 * - ExecuteMode::parallel() → PARALLEL (多线程/多Worker)
 */
class PPPlugin : public IOptionPlugin {
public:
    PPPlugin() = default;
    ~PPPlugin() override = default;
    
    // ========================================
    // IOptionPlugin 接口实现
    // ========================================
    
    std::string getName() const override { return "pp"; }
    std::string getDescription() const override { return "后处理格式测试"; }
    
    void registerOptions(CLI::App& app) override;
    void applyTo(WorkerConfig& config) const override;
    void listTests() const override;
    
    int handlePreActions() override;
    std::vector<WorkerConfig> buildPipelineConfigs(const WorkerConfig& shared_config) override;
    std::string getTestName() const override;
    
    // ========================================
    // 辅助方法
    // ========================================
    
    /**
     * @brief 获取预定义测试参数
     */
    static const std::map<std::string, PPTestParams>& getPredefinedTests();
    
    /**
     * @brief 从 PPTestParams 构建 WorkerConfig
     */
    static WorkerConfig buildConfig(const std::string& path, const PPTestParams& params);

private:
    // ========================================
    // 解析状态（由 CLI11 自动填充）
    // ========================================
    bool show_list_ = false;
    PPTestParams params_;
    std::string input_path_;
    std::string decoder_str_;
    std::string format_str_ = "nv12";
    std::string channel_str_;
    std::string resolution_str_;
    std::string crop_str_;
    std::string color_std_str_ = "bt601";
    bool enable_psnr_ = false;
    bool enable_ssim_ = false;
    double min_psnr_ = -1.0;
    double min_ssim_ = -1.0;
    bool verbose_ = false;
    DataSourceOptions ds_opts_;    ///< DataSource 横切选项
    std::vector<std::string> save_paths_;
    std::vector<int> save_frames_;
    int max_frames_ = 0;
    std::vector<std::string> positional_args_;
};

} // namespace pp
} // namespace test

#endif // PP_PLUGIN_HPP
