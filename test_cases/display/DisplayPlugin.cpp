/**
 * @file DisplayPlugin.cpp
 * @brief 显示输出插件实现
 */

#include "DisplayPlugin.hpp"
#include "../common/third_party/CLI11.hpp"
#include "vendor/taco/display/TacoProDisplayExtension.hpp"
#include "vendor/taco/display/TacoDisplayExtension.hpp"

namespace test {
namespace display {

std::string DisplayPlugin::getName() const {
    return "display";
}

std::string DisplayPlugin::getDescription() const {
    return "显示输出";
}

void DisplayPlugin::registerOptions(CLI::App& app) {
    app.add_option("--vendor", vendor_str_, "显示厂商: tacopro(默认), taco");

    // 共用选项（保留旧参数名，无前缀）
    app.add_option("--fps", target_fps_, "显示刷新帧率 (默认: 30)");
    app.add_flag("--osd", osd_enable_, "启用 OSD 叠加");
    app.add_option("--osd-fps", osd_fps_, "OSD 刷新频率 (默认: 1)");
    app.add_option("--view-type", view_type_, "视图类型: grid(默认), main_sidebar");
    app.add_option("--slot-assignment", slot_assignment_, "通道→slot 映射")->delimiter(',');
    app.add_option("--main-ratio", main_sidebar_ratio_, "main_sidebar 主画面宽度占比 (默认: 0.75)");

    // tacopro 独有
    app.add_option("--screen-width", screen_width_, "屏幕宽度 (tacopro, 默认: 1920)");
    app.add_option("--screen-height", screen_height_, "屏幕高度 (tacopro, 默认: 1080)");
    app.add_option("--bpp", bpp_, "每像素位数 (tacopro, 默认: 32)");

    // taco 独有
    app.add_option("--frame-width", frame_width_, "帧宽度 (taco, 默认: 1920)");
    app.add_option("--frame-height", frame_height_, "帧高度 (taco, 默认: 1080)");
}

void DisplayPlugin::applyTo(WorkerConfig& config) const {
    config.consumer_type.display.enable = true;

    if (vendor_str_ == "taco") {
        auto ext = std::make_unique<TacoDisplayExtension>();
        ext->target_fps = target_fps_;
        ext->screen_width = screen_width_;
        ext->screen_height = screen_height_;
        ext->frame_width = frame_width_;
        ext->frame_height = frame_height_;
        ext->osd_enable = osd_enable_;
        ext->osd_fps = osd_fps_;
        if (!view_type_.empty()) ext->view_type = view_type_;
        if (!slot_assignment_.empty()) ext->slot_assignment = slot_assignment_;
        ext->main_sidebar_ratio = main_sidebar_ratio_;
        config.consumer_type.display.vendor = std::move(ext);
    } else {
        auto ext = std::make_unique<TacoProDisplayExtension>();
        ext->screen_width = screen_width_;
        ext->screen_height = screen_height_;
        ext->bits_per_pixel = bpp_;
        ext->target_fps = target_fps_;
        ext->osd_enable = osd_enable_;
        ext->osd_fps = osd_fps_;
        if (!view_type_.empty()) ext->view_type = view_type_;
        if (!slot_assignment_.empty()) ext->slot_assignment = slot_assignment_;
        ext->main_sidebar_ratio = main_sidebar_ratio_;
        config.consumer_type.display.vendor = std::move(ext);
    }
}

} // namespace display
} // namespace test
