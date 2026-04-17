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
 * - 厂商分发表模式（与 DisplayPlugin 一致）：新增厂商只需加一个 buildXxxDecoder() + 注册一行
 * 
 * 使用示例：
 * @code
 * ./qa_cases pp --format nv12 --channel 0 --input video.mp4
 * ./qa_cases pp --format argb888 --channel 1 --input video.mp4
 * ./qa_cases pp --psnr video.mp4              # HW vs SW 比较
 * ./qa_cases pp --channel 0,1 --psnr video.mp4  # 通道比较 (v2.27)
 * ./qa_cases pp --vendor taco video.mp4        # 指定厂商
 * ./qa_cases pp -h
 * @endcode
 * 
 * @version 6.0 - 消除 PPTestParams，全 Builder + 厂商分发表
 */

#ifndef PP_PLUGIN_HPP
#define PP_PLUGIN_HPP

#include "../common/IOptionPlugin.hpp"
#include "../common/DataSourceOptions.hpp"
#include "consumptionline/BufferConsumerService.hpp"
#include "productionline/worker/config/ConfigBuilders.hpp"
#include "productionline/worker/config/MultiWorkerConfig.hpp"
#include "productionline/worker/config/WorkerConfigs.hpp"

#include <string>
#include <vector>
#include <map>
#include <unordered_map>

namespace test {
namespace pp {

using TestResult = consumer::ConsumeResult;

/**
 * @brief 后处理测试套件
 * 
 * 实现 IOptionPlugin 接口，作为主插件提供完整的后处理测试功能。
 * 
 * 架构设计：
 * - CLI 选项直接绑定到 config_ 的对应成员（消除中间变量）
 * - 厂商分发表模式：vendorBuilders() 注册各厂商的 DecoderConfig 构建方法
 * - 所有配置构建通过 Builder 完成（不直接赋值结构体字段）
 * - 预定义测试表直接存 WorkerConfig（由 WorkerConfigFactory 生成）
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
    // 预定义测试表（直接存 WorkerConfig）
    // ========================================
    
    /**
     * @brief 获取预定义测试参数
     * @return 测试名 → WorkerConfig 映射表
     */
    static const std::map<std::string, WorkerConfig>& getPredefinedTests();

private:
    // ========================================
    // 核心：直接持有 WorkerConfig，CLI 绑定到其成员
    // ========================================
    WorkerConfig config_;
    
    // ========================================
    // 厂商分发表（与 DisplayPlugin 同模式）
    // ========================================
    using DecoderBuilder = WorkerConfig::DecoderConfig (PPPlugin::*)() const;
    
    /**
     * @brief 厂商解码器构建分发表
     * 
     * 新增厂商只需：
     *   1. 新增 buildXxxDecoder() 私有方法
     *   2. 在 vendorBuilders() 的 map 中加一行
     *   3. registerOptions() 中加该厂商独有的 CLI 选项（如有）
     */
    static const std::unordered_map<std::string, DecoderBuilder>& vendorBuilders();
    
    /// TACO 厂商：用 TacoConfigBuilder 构建 PP 后处理配置
    WorkerConfig::DecoderConfig buildTacoDecoder() const;
    
    // ========================================
    // 厂商选择
    // ========================================
    std::string vendor_str_ = "taco";
    
    // ========================================
    // 厂商无关的 PP 概念参数（CLI 字符串中间变量）
    // 原因：目标字段在 TacoConfig 等厂商 Extension 中（vendor 指针后面），
    //       必须通过各厂商 Builder 构建，无法直接绑定到 config_
    // 各厂商 builder 方法内部读取这些值，映射到各自的厂商 Extension
    // ========================================
    std::string format_str_ = "nv12";      ///< 输出格式 → 各厂商 ConfigBuilder
    std::string channel_str_;              ///< 通道选择 → 各厂商 ConfigBuilder
    std::string crop_str_;                 ///< 裁剪区域 → 各厂商 ConfigBuilder
    std::string color_std_str_ = "bt601";  ///< 颜色标准 → 各厂商 ConfigBuilder
    
    // ========================================
    // CLI 便捷入口
    // ========================================
    std::string resolution_str_;           ///< "1920x1080" 便捷写法
    int pp_width_ = 0;                    ///< PP 输出宽度（0=默认1920）
    int pp_height_ = 0;                   ///< PP 输出高度（0=默认1080）
    
    // ========================================
    // 控制变量（非配置数据）
    // ========================================
    bool show_list_ = false;
    std::vector<std::string> positional_args_;
    DataSourceOptions ds_opts_;            ///< DataSource 横切选项
};

} // namespace pp
} // namespace test

#endif // PP_PLUGIN_HPP
