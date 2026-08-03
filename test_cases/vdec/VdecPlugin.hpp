/**
 * @file VdecPlugin.hpp
 * @brief 视频解码测试插件
 * 
 * 封装所有视频解码相关的测试功能，包括：
 * - H.264/H.265/MJPEG 硬件解码
 * - 软件解码
 * - RTSP 流解码
 * - PSNR/SSIM 质量验证
 * 
 * 架构设计：
 * - 实现 IOptionPlugin 接口，作为主执行插件
 * - 使用静态 ExecuteMode 类执行 SINGLE / COMPARE / PARALLEL 模式
 * 
 * 使用示例：
 * @code
 * ./qa_cases vdec --file video.mp4 --codec h264 --width 1920 --height 1080
 * ./qa_cases vdec --rtsp rtsp://192.168.1.100/stream
 * ./qa_cases vdec -h
 * @endcode
 * 
 * @version 5.0 - 重构为 IOptionPlugin 插件架构
 */

#ifndef VDEC_PLUGIN_HPP
#define VDEC_PLUGIN_HPP

#include "../common/IOptionPlugin.hpp"
#include "../common/ExecuteMode.hpp"
#include "../common/DataSourceOptions.hpp"
#include "../common/CompareOptions.hpp"
#include "consumptionline/core/BufferConsumerService.hpp"
#include "productionline/worker/config/MultiWorkerConfig.hpp"
#include "productionline/worker/config/WorkerConfigs.hpp"

#include <string>
#include <vector>
#include <map>

namespace test {
namespace vdec {

using TestResult = consumer::ConsumeResult;

/**
 * @brief 解码测试参数
 */
struct DecodeTestParams {
    std::string codec;           ///< 编解码器 (h264, h265, mjpeg)
    int width;                   ///< 分辨率宽度
    int height;                  ///< 分辨率高度
    double fps;                  ///< 目标帧率
    std::string profile;         ///< profile (main, baseline, high)
    bool use_hardware;           ///< 是否使用硬件解码（默认 true）
    std::string predefined_name; ///< 匹配的预定义测试名称（空表示未使用预定义测试）
    
    DecodeTestParams(
        const std::string& c = "h264",
        int w = 1920, int h = 1080,
        double f = 30.0,
        const std::string& p = "main",
        bool hw = true
    ) : codec(c), width(w), height(h), fps(f), profile(p), use_hardware(hw) {}
    
    /// 是否使用了预定义测试
    bool isPredefined() const { return !predefined_name.empty(); }
};

/**
 * @brief 视频解码测试插件
 * 
 * 实现 IOptionPlugin 接口，作为主执行插件。
 * 
 * 执行模式映射：
 * - SINGLE   → ExecuteMode::single()
 * - COMPARE  → ExecuteMode::compare()  (PSNR/SSIM)
 * - PARALLEL → ExecuteMode::parallel() (多线程/多Worker)
 */
class VdecPlugin : public IOptionPlugin {
public:
    VdecPlugin() = default;
    ~VdecPlugin() override = default;
    
    // ========================================
    // IOptionPlugin 接口实现
    // ========================================
    
    std::string getName() const override { return "vdec"; }
    std::string getDescription() const override { return "视频解码测试"; }
    
    void registerOptions(CLI::App& app) override;
    void applyCliToConfig(WorkerConfig& config) const override;
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
    static const std::map<std::string, DecodeTestParams>& getPredefinedTests();

private:
    DecodeTestParams resolveParams() const {
        DecodeTestParams p = params_;
        if (threads_ > 0) p.profile = "parallel_" + std::to_string(threads_);
        return p;
    }

    bool show_list_ = false;
    
    DecodeTestParams params_;
    std::string input_path_;
    std::vector<std::string> positional_args_;
    std::string decoder_str_;
    std::string resolution_str_;
    
    bool verbose_ = false;
    int threads_ = 0;              ///< 0 = 未指定
    int max_frames_ = -1;          ///< -1 = 无限制
    int loop_count_ = 1;           ///< 数据源循环遍数（默认 1）
    std::string vendor_str_ = "taco";  ///< 解码器厂商（默认 taco，与 DisplayPlugin --vendor 同模式）
    DataSourceOptions ds_opts_;    ///< DataSource 横切选项
    CompareOptions compare_opts_;  ///< COMPARE 横切选项（默认 target=peer）
};

} // namespace vdec
} // namespace test

#endif // VDEC_PLUGIN_HPP
