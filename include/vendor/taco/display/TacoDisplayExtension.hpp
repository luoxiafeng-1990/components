#ifndef TACO_DISPLAY_EXTENSION_HPP
#define TACO_DISPLAY_EXTENSION_HPP

#include "vendor/contracts/DisplayVendorExtension.hpp"
#include <string>
#include <vector>

/**
 * @brief vendor=taco：TACO VO 视频输出管道
 *
 * 独立参数字段（与 TacoProDisplayExtension 不共用结构）。
 */
class TacoDisplayExtension : public IDisplayVendorExtension {
public:
    int target_fps = 30;
    int screen_width = 1920;
    int screen_height = 1080;
    int frame_width = 1920;
    int frame_height = 1080;
    int frame_format = 23;          ///< TA_AV_PIX_FMT_NV12 = 23
    int frame_pool_size = 4;
    std::string view_type = "grid";
    std::vector<int> slot_assignment;
    float main_sidebar_ratio = 0.75f;
    bool osd_enable = false;
    int osd_fps = 1;
    std::string osd_font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    int osd_font_size = 24;

    const char* kind() const noexcept override { return "taco"; }
    std::unique_ptr<IDisplayVendorExtension> clone() const override;
    bool validate(std::string& err) const override;
};

#endif // TACO_DISPLAY_EXTENSION_HPP
