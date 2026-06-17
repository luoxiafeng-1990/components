#ifndef TACO_DECODER_CONFIG_HPP
#define TACO_DECODER_CONFIG_HPP

/**
 * @brief TACO 解码器通道枚举
 * 
 * TACO 解码器支持两个输出通道：
 * - CH0: YUV 格式输出通道
 * - CH1: RGB/YUV 格式输出通道（支持格式转换）
 */
enum class Channel { 
    CH0 = 0,
    CH1 = 1
};

/**
 * @brief TACO 解码器输出格式枚举
 * 
 * 包含 TACO PP（后处理器）支持的所有输出格式。
 * YUV 格式（< 1000）：通道 0 和通道 1 都支持
 * RGB 格式（>= 1000）：仅通道 1 支持
 */
enum class OutputFormat {
    // YUV 格式
    YUV_AUTO = -1,
    YUV_NV12 = 0,
    YUV_NV21 = 1,
    YUV_I420 = 2,
    YUV_YV12 = 3,
    YUV_P010 = 4,
    YUV_NV16 = 5,
    YUV_NV61 = 6,
    YUV_I422 = 7,
    YUV_NV24 = 8,
    YUV_I444 = 9,
    
    // RGB 格式（驱动值见 TacoDecoderExtension::mapOutputFormatToRgbDriverValue）
    RGB_ARGB888 = 1000,
    RGB_ABGR888 = 1001,
    RGB_RGBA888 = 1002,
    RGB_BGRA888 = 1003,
    RGB_RGB888 = 1004,
    RGB_BGR888 = 1005,
    RGB_XRGB888 = 1006,
    RGB_XBGR888 = 1007,
    RGB_RGBX888 = 1008,
    RGB_BGRX888 = 1009,
    RGB_RGB888_PLANAR = 1010,
    RGB_BGR888_PLANAR = 1011,
    RGB_R16G16B16 = 1012,
    RGB_B16G16R16 = 1013,
    RGB_GBRP = 1014,
    RGB_A2R10G10B10 = 1015,
    RGB_A2B10G10R10 = 1016,
    RGB_R10G10B10A2 = 1017,
    RGB_B10G10R10A2 = 1018
};

/**
 * @brief TACO PP 色彩空间转换系数
 * 
 * 用于配置 TACO 后处理器的 YUV↔RGB 转换矩阵。
 * 值直接对应 TACO 驱动层的色彩标准编号。
 */
enum class TacoColorSpace {
    NONE = 0,
    BT601_FULL = 1,
    BT601_LIMITED = 2,
    BT709_FULL = 3,
    BT709_LIMITED = 4,
    BT2020_FULL = 5,
    BT2020_LIMITED = 6
};

/**
 * @brief TACO 硬件解码器专用参数（与 WorkerConfig::DecoderConfig 解耦，便于独立演进）
 *
 * 由 TacoDecoderExtension 持有；通过 tacoDecoderConfig(DecoderConfig&) 访问。
 */
struct TacoConfig {
    bool reorder_disable = true;

    bool ch0_enable = true;
    int ch0_yuv_format = -1;
    int ch0_yuv_std = 1;
    int ch0_crop_x = 0;
    int ch0_crop_y = 0;
    int ch0_crop_width = 0;
    int ch0_crop_height = 0;
    int ch0_scale_width = 0;
    int ch0_scale_height = 0;

    bool ch1_enable = false;
    bool ch1_rgb = false;
    int ch1_rgb_format = 9;
    int ch1_rgb_std = 1;
    int ch1_yuv_format = -1;
    int ch1_yuv_std = 1;
    int ch1_crop_x = 0;
    int ch1_crop_y = 0;
    int ch1_crop_width = 0;
    int ch1_crop_height = 0;
    int ch1_scale_width = 0;
    int ch1_scale_height = 0;

    TacoConfig() = default;
    TacoConfig(const TacoConfig&) = default;
    TacoConfig& operator=(const TacoConfig&) = default;
    TacoConfig(TacoConfig&&) = default;
    TacoConfig& operator=(TacoConfig&&) = default;
};

#endif
