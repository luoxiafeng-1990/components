#ifndef TACO_DECODER_CONFIG_HPP
#define TACO_DECODER_CONFIG_HPP

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
