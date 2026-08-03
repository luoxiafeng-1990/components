/**
 * @file DisplayPlugin.cpp
 * @brief 显示输出插件实现
 *
 * 使用分发表模式：vendorBuilders() 注册所有厂商的 Extension 构建方法，
 * applyCliToConfig() 查表调用，无需 if-else / switch-case。
 * 新增厂商只需加一个 buildXxxExtension() + 在 map 中注册一行。
 */

#include "DisplayPlugin.hpp"
#include "../common/third_party/CLI11.hpp"
#include "vendor/taco/display/TacoProDisplayExtension.hpp"
#include "vendor/taco/display/TacoDisplayExtension.hpp"
#include "consumptionline/config/ConsumerTypeConfigBuilder.hpp"

#include <iostream>


namespace test {
namespace display {

std::string DisplayPlugin::getName() const {
    return "display";
}

std::string DisplayPlugin::getDescription() const {
    return "显示输出";
}

// ========================================
// 厂商分发表
// ========================================

const std::unordered_map<std::string, DisplayPlugin::ExtBuilder>&
DisplayPlugin::vendorBuilders() {
    static const std::unordered_map<std::string, ExtBuilder> map = {
        {"tacopro", &DisplayPlugin::buildTacoProExtension},
        {"taco",    &DisplayPlugin::buildTacoExtension},
    };
    return map;
}

// ========================================
// CLI 选项注册
// ========================================

void DisplayPlugin::registerOptions(CLI::App& app) {
    auto* vendor_opt = app.add_option("--vendor", vendor_str_,
                                      "显示厂商: tacopro(默认), taco");

    app.add_option("--fps", target_fps_, "显示刷新帧率 (默认: 30)");
    auto* display_pp_opt = app.add_option("--display-pp", display_pp_,
        "双 PP 送显通道: 0|1（需同时指定 --vendor；仅多通道时使用）");
    display_pp_opt->needs(vendor_opt);

    app.add_flag("--osd", osd_enable_, "启用 OSD 叠加 (tacopro)");
    app.add_option("--osd-fps", osd_fps_, "OSD 刷新频率 (tacopro, 默认: 1)");
    app.add_option("--view-type", view_type_,
                   "视图类型 (tacopro): grid(默认), main_sidebar");
    app.add_option("--slot-assignment", slot_assignment_,
                   "通道→slot 映射 (tacopro)")->delimiter(',');
    app.add_option("--main-ratio", main_sidebar_ratio_,
                   "main_sidebar 主画面宽度占比 (tacopro, 默认: 0.75)");
    app.add_option("--bpp", bpp_, "每像素位数 (tacopro, 默认: 32)");
}

int DisplayPlugin::handlePreActions() {
    const auto& builders = vendorBuilders();
    if (builders.find(vendor_str_) == builders.end()) {
        std::cerr << "DisplayPlugin: unknown vendor '" << vendor_str_ << "'\n";
        return 1;
    }
    return -1;
}

// ========================================
// applyCliToConfig：查分发表，构建对应厂商 Extension
// ========================================

void DisplayPlugin::applyCliToConfig(WorkerConfig& config) const {
    const auto& builders = vendorBuilders();
    auto it = builders.find(vendor_str_);
    if (it == builders.end()) {
        return;
    }
    config.consumer_type = ConsumerTypeConfigBuilder(config.consumer_type)
        .setDisplayConfig(DisplayConsumerConfigBuilder(config.consumer_type.display)
            .setEnable(true)
            .setVendor((this->*(it->second))())
            .build())
        .build();
}

// ========================================
// 各厂商 Extension 构建
// ========================================

std::unique_ptr<IDisplayVendorExtension> DisplayPlugin::buildTacoProExtension() const {
    auto ext = std::make_unique<TacoProDisplayExtension>();
    ext->bits_per_pixel     = bpp_;
    ext->target_fps         = target_fps_;
    ext->osd_enable         = osd_enable_;
    ext->osd_fps            = osd_fps_;
    ext->display_pp_channel = display_pp_;
    if (!view_type_.empty()) ext->view_type = view_type_;
    if (!slot_assignment_.empty()) ext->slot_assignment = slot_assignment_;
    ext->main_sidebar_ratio = main_sidebar_ratio_;
    return ext;
}

std::unique_ptr<IDisplayVendorExtension> DisplayPlugin::buildTacoExtension() const {
    auto ext = std::make_unique<TacoDisplayExtension>();
    ext->target_fps         = target_fps_;
    ext->osd_enable         = osd_enable_;
    ext->osd_fps            = osd_fps_;
    ext->display_pp_channel = display_pp_;
    if (!view_type_.empty()) ext->view_type = view_type_;
    if (!slot_assignment_.empty()) ext->slot_assignment = slot_assignment_;
    ext->main_sidebar_ratio = main_sidebar_ratio_;
    return ext;
}

} // namespace display
} // namespace test
