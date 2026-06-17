#include "productionline/worker/config/ConfigBuilders.hpp"
#include "productionline/worker/config/MultiWorkerConfig.hpp"
#include "productionline/worker/config/WorkerConfigs.hpp"
#include "vendor/taco/decode/TacoDecoderExtension.hpp"
#include <algorithm>
#include <string>
#include <cstdio>

// ============================================================
// TacoConfigBuilder 实现（从 WorkerConfig.cpp 迁入）
// ============================================================

TacoConfigBuilder& TacoConfigBuilder::setChannels(bool ch0, bool ch1) {
    taco_config_.ch0_enable = ch0;
    taco_config_.ch1_enable = ch1;
    return *this;
}

TacoConfigBuilder& TacoConfigBuilder::setOutputFormat(
    Channel ch,
    OutputFormat format,
    TacoColorSpace std
) {
    int format_value = static_cast<int>(format);
    int std_value = static_cast<int>(std);
    
    // 判断是 RGB 还是 YUV（RGB 格式枚举值 >= 1000）
    bool is_rgb = (format_value >= 1000);
    
    if (ch == Channel::CH0) {
        // 通道0仅支持 YUV
        if (is_rgb) {
            // 通道0不支持 RGB 格式，忽略此配置
            return *this;
        }
        // 设置 YUV 格式
        taco_config_.ch0_yuv_format = format_value;
        taco_config_.ch0_yuv_std = std_value;
        
    } else if (ch == Channel::CH1) {
        // 通道1支持 RGB 和 YUV
        taco_config_.ch1_rgb = is_rgb;
        
        if (is_rgb) {
            taco_config_.ch1_rgb_format = TacoDecoderExtension::mapOutputFormatToRgbDriverValue(format);
            taco_config_.ch1_rgb_std = std_value;
        } else {
            // YUV 格式
            taco_config_.ch1_yuv_format = format_value;
            taco_config_.ch1_yuv_std = std_value;
        }
    }
    
    return *this;
}

TacoConfigBuilder& TacoConfigBuilder::setCrop(Channel ch, int x, int y, int width, int height) {
    if (ch == Channel::CH0) {
        taco_config_.ch0_crop_x = x;
        taco_config_.ch0_crop_y = y;
        taco_config_.ch0_crop_width = width;
        taco_config_.ch0_crop_height = height;
    } else if (ch == Channel::CH1) {
        taco_config_.ch1_crop_x = x;
        taco_config_.ch1_crop_y = y;
        taco_config_.ch1_crop_width = width;
        taco_config_.ch1_crop_height = height;
    }
    return *this;
}

TacoConfigBuilder& TacoConfigBuilder::setScale(Channel ch, int width, int height) {
    if (ch == Channel::CH0) {
        taco_config_.ch0_scale_width = width;
        taco_config_.ch0_scale_height = height;
    } else if (ch == Channel::CH1) {
        taco_config_.ch1_scale_width = width;
        taco_config_.ch1_scale_height = height;
    }
    return *this;
}

TacoConfig TacoConfigBuilder::build() const {
    return taco_config_;
}

// ============================================================
// TacoConfigBuilder 静态辅助函数实现
// ============================================================

OutputFormat TacoConfigBuilder::mapFormatNameToEnum(std::string_view format_name) {
    std::string name(format_name);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // YUV 格式
    if (name == "auto" || name == "yuv_auto") return OutputFormat::YUV_AUTO;
    if (name == "nv12") return OutputFormat::YUV_NV12;
    if (name == "nv21") return OutputFormat::YUV_NV21;
    if (name == "i420" || name == "yuv420p") return OutputFormat::YUV_I420;
    if (name == "yv12") return OutputFormat::YUV_YV12;
    if (name == "p010") return OutputFormat::YUV_P010;
    if (name == "nv16") return OutputFormat::YUV_NV16;
    if (name == "nv61") return OutputFormat::YUV_NV61;
    if (name == "i422" || name == "yuv422p") return OutputFormat::YUV_I422;
    if (name == "nv24") return OutputFormat::YUV_NV24;
    if (name == "i444" || name == "yuv444p") return OutputFormat::YUV_I444;

    // RGB 格式（含 ffmpeg 风格别名）
    if (name == "argb888" || name == "argb") return OutputFormat::RGB_ARGB888;
    if (name == "abgr888" || name == "abgr") return OutputFormat::RGB_ABGR888;
    if (name == "rgba888" || name == "rgba") return OutputFormat::RGB_RGBA888;
    if (name == "bgra888" || name == "bgra") return OutputFormat::RGB_BGRA888;
    if (name == "rgb888" || name == "rgb24") return OutputFormat::RGB_RGB888;
    if (name == "bgr888" || name == "bgr24") return OutputFormat::RGB_BGR888;
    if (name == "xrgb888" || name == "0rgb") return OutputFormat::RGB_XRGB888;
    if (name == "xbgr888" || name == "0bgr") return OutputFormat::RGB_XBGR888;
    if (name == "rgbx888" || name == "rgb0") return OutputFormat::RGB_RGBX888;
    if (name == "bgrx888" || name == "bgr0") return OutputFormat::RGB_BGRX888;
    if (name == "rgb888_planar") return OutputFormat::RGB_RGB888_PLANAR;
    if (name == "bgr888_planar") return OutputFormat::RGB_BGR888_PLANAR;
    if (name == "r16g16b16") return OutputFormat::RGB_R16G16B16;
    if (name == "b16g16r16") return OutputFormat::RGB_B16G16R16;
    if (name == "gbrp") return OutputFormat::RGB_GBRP;
    if (name == "argb2101010" || name == "a2r10g10b10" || name == "rgbx101010" || name == "rgb101010")
        return OutputFormat::RGB_A2R10G10B10;
    if (name == "abgr2101010" || name == "a2b10g10r10" || name == "bgrx101010" || name == "bgr101010")
        return OutputFormat::RGB_A2B10G10R10;
    if (name == "rgba2101010" || name == "r10g10b10a2") return OutputFormat::RGB_R10G10B10A2;
    if (name == "bgra2101010" || name == "b10g10r10a2") return OutputFormat::RGB_B10G10R10A2;

    fprintf(stderr, "[WARN] mapFormatNameToEnum: unrecognized format \"%s\", fallback to YUV_AUTO\n",
            std::string(format_name).c_str());
    return OutputFormat::YUV_AUTO;
}

TacoColorSpace TacoConfigBuilder::mapColorStdNameToEnum(std::string_view std_name) {
    if (std_name == "none") return TacoColorSpace::NONE;
    if (std_name == "bt601") return TacoColorSpace::BT601_FULL;
    if (std_name == "bt601_l" || std_name == "bt601_limited") return TacoColorSpace::BT601_LIMITED;
    if (std_name == "bt709") return TacoColorSpace::BT709_FULL;
    if (std_name == "bt709_l" || std_name == "bt709_limited") return TacoColorSpace::BT709_LIMITED;
    if (std_name == "bt2020") return TacoColorSpace::BT2020_FULL;
    if (std_name == "bt2020_l" || std_name == "bt2020_limited") return TacoColorSpace::BT2020_LIMITED;
    
    return TacoColorSpace::BT601_FULL;
}

std::string_view TacoConfigBuilder::mapFormatEnumToName(OutputFormat format) {
    switch (format) {
        // YUV 格式
        case OutputFormat::YUV_AUTO: return "yuv_auto";
        case OutputFormat::YUV_NV12: return "nv12";
        case OutputFormat::YUV_NV21: return "nv21";
        case OutputFormat::YUV_I420: return "i420";
        case OutputFormat::YUV_YV12: return "yv12";
        case OutputFormat::YUV_P010: return "p010";
        case OutputFormat::YUV_NV16: return "nv16";
        case OutputFormat::YUV_NV61: return "nv61";
        case OutputFormat::YUV_I422: return "i422";
        case OutputFormat::YUV_NV24: return "nv24";
        case OutputFormat::YUV_I444: return "i444";
        
        // RGB 格式
        case OutputFormat::RGB_ARGB888: return "argb888";
        case OutputFormat::RGB_ABGR888: return "abgr888";
        case OutputFormat::RGB_RGBA888: return "rgba888";
        case OutputFormat::RGB_BGRA888: return "bgra888";
        case OutputFormat::RGB_RGB888: return "rgb888";
        case OutputFormat::RGB_BGR888: return "bgr888";
        case OutputFormat::RGB_XRGB888: return "xrgb888";
        case OutputFormat::RGB_XBGR888: return "xbgr888";
        case OutputFormat::RGB_RGBX888: return "rgbx888";
        case OutputFormat::RGB_BGRX888: return "bgrx888";
        case OutputFormat::RGB_RGB888_PLANAR: return "rgb888_planar";
        case OutputFormat::RGB_BGR888_PLANAR: return "bgr888_planar";
        case OutputFormat::RGB_R16G16B16: return "r16g16b16";
        case OutputFormat::RGB_B16G16R16: return "b16g16r16";
        case OutputFormat::RGB_GBRP: return "gbrp";
        case OutputFormat::RGB_A2R10G10B10: return "argb2101010";
        case OutputFormat::RGB_A2B10G10R10: return "abgr2101010";
        case OutputFormat::RGB_R10G10B10A2: return "rgba2101010";
        case OutputFormat::RGB_B10G10R10A2: return "bgra2101010";
        
        default: return "unknown";
    }
}

std::string_view TacoConfigBuilder::mapColorStdEnumToName(TacoColorSpace std) {
    switch (std) {
        case TacoColorSpace::NONE: return "none";
        case TacoColorSpace::BT601_FULL: return "bt601";
        case TacoColorSpace::BT601_LIMITED: return "bt601_limited";
        case TacoColorSpace::BT709_FULL: return "bt709";
        case TacoColorSpace::BT709_LIMITED: return "bt709_limited";
        case TacoColorSpace::BT2020_FULL: return "bt2020";
        case TacoColorSpace::BT2020_LIMITED: return "bt2020_limited";
        default: return "unknown";
    }
}

int TacoConfigBuilder::mapEnumToRgbDriverValue(OutputFormat format) {
    return TacoDecoderExtension::mapOutputFormatToRgbDriverValue(format);
}
