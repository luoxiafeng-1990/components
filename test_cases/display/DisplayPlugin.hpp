/**
 * @file DisplayPlugin.hpp
 * @brief 显示输出插件
 *
 * 作为独立子命令 display 注册，管理显示输出相关的命令行参数解析和配置注入。
 * 通过 --vendor 选择厂商（默认 tacopro），参数名不加前缀，旧命令兼容。
 *
 * 用法：
 * @code
 * ./qa_cases vdec --file video.mp4 display --fps 60
 * ./qa_cases vdec --file video.mp4 display --vendor taco --fps 30
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
    std::string vendor_str_ = "tacopro";

    // 共用选项（两厂商均有，不加前缀）
    int target_fps_ = 30;
    bool osd_enable_ = false;
    int osd_fps_ = 1;
    std::string view_type_;
    std::vector<int> slot_assignment_;
    float main_sidebar_ratio_ = 0.75f;

    // tacopro 独有
    int screen_width_ = 1920;
    int screen_height_ = 1080;
    int bpp_ = 32;

    // taco 独有
    int frame_width_ = 1920;
    int frame_height_ = 1080;
};

} // namespace display
} // namespace test

#endif // TEST_DISPLAY_PLUGIN_HPP
