/**
 * @file RecordPlugin.hpp
 * @brief 录制测试套件
 * 
 * 封装所有录制相关的测试功能，包括：
 * - RTSP 流录制
 * - 文件重封装
 * - 多格式输出
 * 
 * 架构设计：
 * - 实现 IOptionPlugin 接口，作为主插件（canExecute = true）
 * - 使用 ExecuteMode 静态工具类执行 SINGLE 模式
 * - 录制测试使用 ExecuteMode::SINGLE + CONSUME_SAVE_ENCODED
 * 
 * 使用示例：
 * @code
 * ./qa_cases record --input rtsp://192.168.1.100/stream --output /tmp/test.mp4
 * ./qa_cases record --input video.mp4 --output /tmp/remux.mkv
 * ./qa_cases record -h
 * @endcode
 * 
 * @version 6.0 - 迁移到 CLI11
 */

#ifndef RECORD_PLUGIN_HPP
#define RECORD_PLUGIN_HPP

#include "../common/IOptionPlugin.hpp"
#include "../common/ExecuteMode.hpp"
#include "consumptionline/core/BufferConsumerService.hpp"
#include "productionline/worker/config/MultiWorkerConfig.hpp"
#include "productionline/worker/config/WorkerConfigs.hpp"

#include <string>
#include <vector>
#include <map>

namespace CLI { class App; }

namespace test {
namespace record {

// 类型别名：使用新架构的 ConsumeResult
using TestResult = consumer::ConsumeResult;

/**
 * @brief 录制测试参数
 */
struct RecordTestParams {
    std::string format;         ///< 输出格式 (mp4, mkv, mov, ts, flv, avi)
    double duration;            ///< 录制时长（秒）
    
    RecordTestParams(
        const std::string& fmt = "mp4",
        double dur = 10.0
    ) : format(fmt), duration(dur) {}
};

/**
 * @brief 录制测试套件
 * 
 * 实现 IOptionPlugin 接口，作为主插件提供完整的录制测试功能。
 * 
 * 架构设计：
 * - 所有录制测试使用 ExecuteMode::SINGLE
 * - 消费标志：CONSUME_SAVE_ENCODED
 */
class RecordPlugin : public IOptionPlugin {
public:
    RecordPlugin() = default;
    ~RecordPlugin() override = default;
    
    // ========================================
    // IOptionPlugin 接口实现
    // ========================================
    
    std::string getName() const override { return "record"; }
    std::string getDescription() const override { return "流录制测试"; }
    
    void registerOptions(CLI::App& app) override;
    void applyTo(WorkerConfig& config) const override;
    void listTests() const override;
    
    int handlePreActions() override;
    std::vector<WorkerConfig> buildPipelineConfigs(const WorkerConfig& shared_config) override;
    std::string getTestName() const override;
    
    /**
     * @brief 获取预定义测试参数
     */
    static const std::map<std::string, RecordTestParams>& getPredefinedTests();

private:
    // ========================================
    // 解析状态（由 CLI11 自动填充）
    // ========================================
    static const std::vector<std::string>& getAllFormats();

    bool show_list_ = false;
    std::string input_path_;
    std::string output_path_;
    RecordTestParams params_;
    bool verbose_ = false;
    bool all_formats_ = false;
    std::vector<std::string> positional_args_;
};

} // namespace record
} // namespace test

#endif // RECORD_PLUGIN_HPP
