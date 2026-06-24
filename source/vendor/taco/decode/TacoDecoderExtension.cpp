#include "vendor/taco/decode/TacoDecoderExtension.hpp"
#include "vendor/contracts/DecoderVendorRegistry.hpp"
#include <algorithm>
#include <cstdio>

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

std::unique_ptr<IDecoderVendorExtension> TacoDecoderExtension::clone() const {
    return makeTacoDecoderExtension(config);
}

bool TacoDecoderExtension::validate(std::string& err) const {
    if (config.ch0_yuv_format < -1) {
        err = "TacoConfig: ch0_yuv_format < -1";
        return false;
    }
    if (config.ch1_yuv_format < -1) {
        err = "TacoConfig: ch1_yuv_format < -1";
        return false;
    }
    return true;
}

double TacoDecoderExtension::getChannelBytesPerPixel(
    int channel, void* priv_data, int pix_fmt) const
{
    int64_t value = 0;

    if (channel == 0) {
        if (av_opt_get_int(priv_data, "ch0_enable", 0, &value) < 0 || value == 0) {
            return 0.0;
        }
        if (pix_fmt >= 0) {
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(
                static_cast<AVPixelFormat>(pix_fmt));
            if (desc) {
                return av_get_bits_per_pixel(desc) / 8.0;
            }
        }
        return 1.5;
    }

    if (channel == 1) {
        if (av_opt_get_int(priv_data, "ch1_enable", 0, &value) < 0 || value == 0) {
            return 0.0;
        }
        if (av_opt_get_int(priv_data, "ch1_rgb", 0, &value) < 0 || value == 0) {
            return 1.5;
        }
        int64_t rgb_format = 0;
        if (av_opt_get_int(priv_data, "ch1_rgb_format", 0, &rgb_format) < 0) {
            return 4.0;
        }
        OutputFormat format = mapRgbDriverValueToOutputFormat(static_cast<int>(rgb_format));
        return getBytesPerPixelFromOutputFormat(format);
    }

    return 0.0;
}

int TacoDecoderExtension::getOutputWidth(int channel) const {
    if (channel == 0) return config.ch0_scale_width;
    if (channel == 1) return config.ch1_scale_width;
    return 0;
}

int TacoDecoderExtension::getOutputHeight(int channel) const {
    if (channel == 0) return config.ch0_scale_height;
    if (channel == 1) return config.ch1_scale_height;
    return 0;
}

bool TacoDecoderExtension::applyToCodecContext(
    void* priv_data, int source_width, int source_height)
{
    if (!priv_data) {
        fprintf(stderr, "[TACO] ERROR: priv_data is NULL, cannot configure PP channels\n");
        return false;
    }

    int ret;

    // ── 解码器行为：B 帧重排序 ──
    bool actual_reorder_disable;
    switch (config.reorder_mode) {
        case TacoConfig::ReorderMode::ON:   actual_reorder_disable = false; break;
        case TacoConfig::ReorderMode::OFF:  actual_reorder_disable = true;  break;
        default: /* AUTO */                 actual_reorder_disable = config.reorder_disable_resolved; break;
    }
    ret = av_opt_set_int(priv_data, "reorder_disable",
                         actual_reorder_disable ? 1 : 0, 0);
    const char* mode_str = (config.reorder_mode == TacoConfig::ReorderMode::AUTO) ? "AUTO" :
                           (config.reorder_mode == TacoConfig::ReorderMode::ON)   ? "ON" : "OFF";
    fprintf(stderr, "[TACO]   reorder_disable=%d (mode=%s): %s\n",
            actual_reorder_disable ? 1 : 0, mode_str, ret < 0 ? "FAILED" : "OK");

    // ── 通道启用 ──
    ret = av_opt_set_int(priv_data, "ch0_enable",
                         config.ch0_enable ? 1 : 0, 0);
    fprintf(stderr, "[TACO]   ch0_enable=%d: %s\n",
            config.ch0_enable ? 1 : 0, ret < 0 ? "FAILED" : "OK");

    ret = av_opt_set_int(priv_data, "ch1_enable",
                         config.ch1_enable ? 1 : 0, 0);
    fprintf(stderr, "[TACO]   ch1_enable=%d: %s\n",
            config.ch1_enable ? 1 : 0, ret < 0 ? "FAILED" : "OK");

    // ========== 通道0 PP 配置 ==========

    // ch0 裁剪：钳制到实际输入分辨率
    if (config.ch0_crop_width > 0 && config.ch0_crop_height > 0) {
        int crop_x = std::min(config.ch0_crop_x, source_width > 0 ? source_width - 1 : 0);
        int crop_y = std::min(config.ch0_crop_y, source_height > 0 ? source_height - 1 : 0);
        int crop_w = std::min(config.ch0_crop_width, source_width - crop_x);
        int crop_h = std::min(config.ch0_crop_height, source_height - crop_y);
        if (crop_w <= 0) crop_w = source_width;
        if (crop_h <= 0) crop_h = source_height;

        av_opt_set_int(priv_data, "ch0_crop_x", crop_x, 0);
        av_opt_set_int(priv_data, "ch0_crop_y", crop_y, 0);
        av_opt_set_int(priv_data, "ch0_crop_width", crop_w, 0);
        av_opt_set_int(priv_data, "ch0_crop_height", crop_h, 0);
        fprintf(stderr, "[TACO]   ch0_crop: config=(%d,%d,%d,%d) clamped=(%d,%d,%d,%d) input=(%dx%d)\n",
                config.ch0_crop_x, config.ch0_crop_y, config.ch0_crop_width, config.ch0_crop_height,
                crop_x, crop_y, crop_w, crop_h, source_width, source_height);
    }

    // ch0 缩放：PP 硬件不支持放大 (uscale_support={0,0,0,0})
    // 参见 libdec24/software/source/common/vpu_features_list.h
    if (config.ch0_scale_width > 0 && config.ch0_scale_height > 0) {
        if (config.ch0_scale_width > source_width || config.ch0_scale_height > source_height) {
            fprintf(stderr, "[TACO] ERROR: ch0 scale %dx%d exceeds source %dx%d — "
                    "HW PP cannot upscale (uscale_support=0)\n",
                    config.ch0_scale_width, config.ch0_scale_height,
                    source_width, source_height);
            return false;
        }
        av_opt_set_int(priv_data, "ch0_scale_width", config.ch0_scale_width, 0);
        av_opt_set_int(priv_data, "ch0_scale_height", config.ch0_scale_height, 0);
        fprintf(stderr, "[TACO]   ch0_scale: (%d, %d)\n",
                config.ch0_scale_width, config.ch0_scale_height);
    }

    // ========== 通道1 PP 配置 ==========

    // ch1 裁剪：钳制到实际输入分辨率
    if (config.ch1_crop_width > 0 && config.ch1_crop_height > 0) {
        int crop_x = std::min(config.ch1_crop_x, source_width > 0 ? source_width - 1 : 0);
        int crop_y = std::min(config.ch1_crop_y, source_height > 0 ? source_height - 1 : 0);
        int crop_w = std::min(config.ch1_crop_width, source_width - crop_x);
        int crop_h = std::min(config.ch1_crop_height, source_height - crop_y);
        if (crop_w <= 0) crop_w = source_width;
        if (crop_h <= 0) crop_h = source_height;

        av_opt_set_int(priv_data, "ch1_crop_x", crop_x, 0);
        av_opt_set_int(priv_data, "ch1_crop_y", crop_y, 0);
        av_opt_set_int(priv_data, "ch1_crop_width", crop_w, 0);
        av_opt_set_int(priv_data, "ch1_crop_height", crop_h, 0);
        fprintf(stderr, "[TACO]   ch1_crop: config=(%d,%d,%d,%d) clamped=(%d,%d,%d,%d) input=(%dx%d)\n",
                config.ch1_crop_x, config.ch1_crop_y, config.ch1_crop_width, config.ch1_crop_height,
                crop_x, crop_y, crop_w, crop_h, source_width, source_height);
    }

    // ch1 缩放：PP 硬件不支持放大
    if (config.ch1_scale_width > 0 && config.ch1_scale_height > 0) {
        if (config.ch1_scale_width > source_width || config.ch1_scale_height > source_height) {
            fprintf(stderr, "[TACO] ERROR: ch1 scale %dx%d exceeds source %dx%d — "
                    "HW PP cannot upscale (uscale_support=0)\n",
                    config.ch1_scale_width, config.ch1_scale_height,
                    source_width, source_height);
            return false;
        }
        av_opt_set_int(priv_data, "ch1_scale_width", config.ch1_scale_width, 0);
        av_opt_set_int(priv_data, "ch1_scale_height", config.ch1_scale_height, 0);
        fprintf(stderr, "[TACO]   ch1_scale: (%d, %d)\n",
                config.ch1_scale_width, config.ch1_scale_height);
    }

    // ch1 RGB 格式
    ret = av_opt_set_int(priv_data, "ch1_rgb",
                         config.ch1_rgb ? 1 : 0, 0);
    fprintf(stderr, "[TACO]   ch1_rgb=%d: %s\n",
            config.ch1_rgb ? 1 : 0, ret < 0 ? "FAILED" : "OK");

    if (config.ch1_rgb && config.ch1_rgb_format < 0) {
        fprintf(stderr, "[TACO] ERROR: unsupported RGB format (ch1_rgb_format=%d)\n",
                config.ch1_rgb_format);
        return false;
    }
    if (config.ch1_rgb && config.ch1_rgb_format > 0) {
        ret = av_opt_set_int(priv_data, "ch1_rgb_format",
                             config.ch1_rgb_format, 0);
        fprintf(stderr, "[TACO]   ch1_rgb_format=%d: %s\n",
                config.ch1_rgb_format, ret < 0 ? "FAILED" : "OK");
    }

    // ch1 色彩标准
    if (config.ch1_rgb && config.ch1_rgb_std > 0) {
        ret = av_opt_set_int(priv_data, "ch1_rgb_std",
                             config.ch1_rgb_std, 0);
        fprintf(stderr, "[TACO]   ch1_rgb_std=%d: %s\n",
                config.ch1_rgb_std, ret < 0 ? "FAILED" : "OK");
    }

    return true;
}

void TacoDecoderExtension::autoConfigureFromCodecParams(
    int codec_id, int profile, int video_delay)
{
    // 非 AUTO 模式：用户强制指定，不自动探测
    if (config.reorder_mode != TacoConfig::ReorderMode::AUTO) {
        return;
    }

    bool has_b_frames = false;

    // 依据1：容器元数据标记 video_delay > 0 → 确定有 B 帧
    if (video_delay > 0) {
        has_b_frames = true;
    }

    // 依据2：H.264 Profile 判断
    //   Baseline(66) / Constrained Baseline(578) → 规范禁止 B 帧
    //   Main(77) / High(100) / 其他 → 可能有 B 帧
    // AV_CODEC_ID_H264 = 27
    if (codec_id == 27) {
        if (profile != 66 && profile != 578) {
            has_b_frames = true;
        }
    }

    // 依据3：H.265/HEVC Main profile 及以上基本都有 B 帧
    // AV_CODEC_ID_HEVC = 173
    if (codec_id == 173) {
        has_b_frames = true;
    }

    // 结论：有 B 帧 → 必须开启 reorder（reorder_disable = false）
    //       无 B 帧 → 关闭 reorder 降低延迟（reorder_disable = true）
    config.reorder_disable_resolved = !has_b_frames;

    fprintf(stderr, "[TACO] B-frame auto-detect: codec_id=%d, profile=%d, "
            "video_delay=%d → has_b_frames=%s → reorder_disable=%d\n",
            codec_id, profile, video_delay,
            has_b_frames ? "true" : "false",
            config.reorder_disable_resolved ? 1 : 0);
}

TacoDecoderExtension* tacoVendorExtension(WorkerConfig::DecoderConfig& d) {
    if (!d.vendor) {
        return nullptr;
    }
    if (std::strcmp(d.vendor->kind(), "taco") != 0) {
        return nullptr;
    }
    return static_cast<TacoDecoderExtension*>(d.vendor.get());
}

const TacoDecoderExtension* tacoVendorExtension(const WorkerConfig::DecoderConfig& d) {
    if (!d.vendor) {
        return nullptr;
    }
    if (std::strcmp(d.vendor->kind(), "taco") != 0) {
        return nullptr;
    }
    return static_cast<const TacoDecoderExtension*>(d.vendor.get());
}

TacoConfig* tacoDecoderConfig(WorkerConfig::DecoderConfig& d) {
    TacoDecoderExtension* e = tacoVendorExtension(d);
    return e ? &e->config : nullptr;
}

const TacoConfig* tacoDecoderConfig(const WorkerConfig::DecoderConfig& d) {
    const TacoDecoderExtension* e = tacoVendorExtension(d);
    return e ? &e->config : nullptr;
}

std::unique_ptr<TacoDecoderExtension> makeTacoDecoderExtension(const TacoConfig& cfg) {
    auto p = std::make_unique<TacoDecoderExtension>();
    p->config = cfg;
    return p;
}

static void register_taco_decoder_vendor_impl() {
    DecoderVendorRegistry::instance().registerVendor("taco", []() {
        return std::make_unique<TacoDecoderExtension>();
    });
}

namespace {

struct TacoVendorRegistrar {
    TacoVendorRegistrar() { register_taco_decoder_vendor_impl(); }
};

static TacoVendorRegistrar g_taco_vendor_registrar;

}  // namespace

extern "C" void register_taco_decoder_vendor() {
    register_taco_decoder_vendor_impl();
}
