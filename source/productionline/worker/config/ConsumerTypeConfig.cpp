#include "consumptionline/config/ConsumerTypeConfig.hpp"

// ============================================================
// OpencvType 拷贝构造 / 赋值（成员多，不宜在头文件内联展开）
// ============================================================

ConsumerTypeConfig::OpencvType::OpencvType(const OpencvType& o)
    : enable(o.enable), op_type(o.op_type)
    , assert_mode(o.assert_mode), min_fps(o.min_fps)
    , resize(o.resize), crop(o.crop), morph(o.morph)
    , sobel(o.sobel), canny(o.canny), laplacian(o.laplacian)
    , translate(o.translate), rotate(o.rotate), perspective(o.perspective)
    , draw_line(o.draw_line), draw_rect(o.draw_rect), put_text(o.put_text)
    , gaussian_blur(o.gaussian_blur), threshold(o.threshold)
    , split_merge(o.split_merge), cvtcolor(o.cvtcolor)
    , imwrite(o.imwrite)
    , vendor(o.vendor ? o.vendor->clone() : nullptr) {}

ConsumerTypeConfig::OpencvType&
ConsumerTypeConfig::OpencvType::operator=(const OpencvType& o) {
    if (this != &o) {
        enable = o.enable; op_type = o.op_type;
        assert_mode = o.assert_mode; min_fps = o.min_fps;
        resize = o.resize; crop = o.crop; morph = o.morph;
        sobel = o.sobel; canny = o.canny; laplacian = o.laplacian;
        translate = o.translate; rotate = o.rotate; perspective = o.perspective;
        draw_line = o.draw_line; draw_rect = o.draw_rect; put_text = o.put_text;
        gaussian_blur = o.gaussian_blur; threshold = o.threshold;
        split_merge = o.split_merge; cvtcolor = o.cvtcolor;
        imwrite = o.imwrite;
        vendor = o.vendor ? o.vendor->clone() : nullptr;
    }
    return *this;
}

// ============================================================
// ConsumerTypeConfig::inheritCompanionSettings 实现
// ============================================================

void ConsumerTypeConfig::inheritCompanionSettings(const ConsumerTypeConfig& shared) {
    if (shared.display.enable && !display.enable)
        display = shared.display;

    if (shared.npu_inference.enable && !npu_inference.enable)
        npu_inference = shared.npu_inference;

    if ((shared.compare.enable_psnr || shared.compare.enable_ssim)
        && !compare.enable_psnr && !compare.enable_ssim)
        compare = shared.compare;

    if (shared.verbose)
        verbose = true;
}
