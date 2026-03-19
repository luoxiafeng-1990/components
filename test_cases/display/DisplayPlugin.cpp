/**
 * @file DisplayPlugin.cpp
 * @brief 显示输出插件实现
 */

#include "DisplayPlugin.hpp"
#include "../common/third_party/CLI11.hpp"

namespace test {
namespace display {

std::string DisplayPlugin::getName() const {
    return "display";
}

std::string DisplayPlugin::getDescription() const {
    return "显示输出";
}

void DisplayPlugin::registerOptions(CLI::App& app) {
    app.add_option("--mode", mode_str_, "显示模式: shared_fb(默认), vo(taco-vo)");
    app.add_option("--fps", target_fps_, "显示刷新帧率 (默认: 30)");
    app.add_flag("--osd", osd_enable_, "启用 OSD 叠加");
    app.add_option("--osd-fps", osd_fps_, "OSD 刷新频率 (默认: 1)");
    app.add_option("--view-type", view_type_, "视图类型: grid(默认), main_sidebar");
    app.add_option("--slot-assignment", slot_assignment_, "通道→slot 映射")->delimiter(',');
    app.add_option("--main-ratio", main_sidebar_ratio_, "main_sidebar 主画面宽度占比 (默认: 0.75)");
}

void DisplayPlugin::applyTo(WorkerConfig& config) const {
    using DisplayMode = WorkerConfig::ConsumerTypeConfig::DisplayType::DisplayMode;

    config.consumer_type.display.enable = true;

    if (mode_str_ == "vo" || mode_str_ == "taco-vo")
        config.consumer_type.display.mode = DisplayMode::TACO_VO;
    else
        config.consumer_type.display.mode = DisplayMode::SHARED_FB;

    config.consumer_type.display.taco_vo.target_fps      = target_fps_;
    config.consumer_type.display.taco_vo.osd_enable      = osd_enable_;
    config.consumer_type.display.taco_vo.osd_fps         = osd_fps_;
    if (!view_type_.empty())
        config.consumer_type.display.taco_vo.view_type   = view_type_;
    if (!slot_assignment_.empty())
        config.consumer_type.display.taco_vo.slot_assignment = slot_assignment_;
    config.consumer_type.display.taco_vo.main_sidebar_ratio = main_sidebar_ratio_;
}

} // namespace display
} // namespace test
