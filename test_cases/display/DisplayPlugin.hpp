/**
 * @file DisplayPlugin.hpp
 * @brief 显示输出插件
 *
 * 作为独立子命令 display 注册，管理显示输出相关的命令行参数解析和配置注入。
 * 使用 display 子命令即表示启用显示输出，无需额外 -d flag。
 *
 * 用法：
 * @code
 * ./qa_cases vdec --file video.mp4 display --display-mode vo --display-fps 60
 * @endcode
 */

#ifndef TEST_DISPLAY_PLUGIN_HPP
#define TEST_DISPLAY_PLUGIN_HPP

#include "../common/IOptionPlugin.hpp"

#include <string>
#include <vector>

namespace CLI { class App; }

namespace test {
namespace display {

class DisplayPlugin : public IOptionPlugin {
public:
    std::string getName() const override;
    std::string getDescription() const override;

    void registerOptions(CLI::App& app) override;
    void applyTo(WorkerConfig& config) const override;

private:
    std::string       mode_str_           = "shared_fb";
    int               target_fps_         = 30;
    bool              osd_enable_         = false;
    int               osd_fps_            = 1;
    std::string       view_type_;
    std::vector<int>  slot_assignment_;
    float             main_sidebar_ratio_ = 0.75f;
    /// 0 = 使用默认 1 路（applyTo 写入 taco_vo.max_channels）；>0 为指定路数
    int               max_channels_       = 0;
};

} // namespace display
} // namespace test

#endif // TEST_DISPLAY_PLUGIN_HPP
