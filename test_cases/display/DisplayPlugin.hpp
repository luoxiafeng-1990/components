/**
 * @file DisplayPlugin.hpp
 * @brief 显示输出插件
 *
 * 作为独立子命令 display 注册，管理显示输出相关的命令行参数解析和配置注入。
 * 通过 --vendor 选择厂商（默认 tacopro），参数名不加前缀，旧命令兼容。
 *
 * 新增厂商只需：
 *   1. 新增 buildXxxExtension() 私有方法
 *   2. 在 vendorBuilders() 的 map 中加一行
 *   3. registerOptions() 中加该厂商独有的 CLI 选项
 *
 * 用法：
 * @code
 * ./qa_cases vdec --file video.mp4 display --fps 60
 * ./qa_cases vdec --file video.mp4 display --vendor taco --fps 30
 * ./qa_cases vdec --file video.mp4 --vendor taco --ch0 --ch1 \
 *     display --vendor taco --display-pp 0
 * @endcode
 */

#ifndef TEST_DISPLAY_PLUGIN_HPP
#define TEST_DISPLAY_PLUGIN_HPP

#include "../common/IOptionPlugin.hpp"
#include "vendor/contracts/DisplayVendorExtension.hpp"

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>

namespace CLI { class App; }

namespace test {
namespace display {

class DisplayPlugin : public IOptionPlugin {
public:
    std::string getName() const override;
    std::string getDescription() const override;

    void registerOptions(CLI::App& app) override;
    int handlePreActions() override;
    void applyTo(WorkerConfig& config) const override;

private:
    using ExtBuilder = std::unique_ptr<IDisplayVendorExtension>(DisplayPlugin::*)() const;
    static const std::unordered_map<std::string, ExtBuilder>& vendorBuilders();

    std::unique_ptr<IDisplayVendorExtension> buildTacoProExtension() const;
    std::unique_ptr<IDisplayVendorExtension> buildTacoExtension() const;

    std::string vendor_str_ = "tacopro";

    // 共用
    int target_fps_ = 30;
    int display_pp_ = -1;

    // tacopro 独有（taco-vo 路径忽略：无 framebuffer stitch / OSD）
    int bpp_ = 32;
    bool osd_enable_ = false;
    int osd_fps_ = 1;
    std::string view_type_;
    std::vector<int> slot_assignment_;
    float main_sidebar_ratio_ = 0.75f;
};

} // namespace display
} // namespace test

#endif // TEST_DISPLAY_PLUGIN_HPP
