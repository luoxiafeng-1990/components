#ifndef TACO_DECODER_EXTENSION_HPP
#define TACO_DECODER_EXTENSION_HPP

#include "vendor/contracts/DecoderVendorExtension.hpp"
#include "vendor/taco/decode/TacoDecoderConfig.hpp"
#include "productionline/worker/config/MultiWorkerConfig.hpp"
#include "productionline/worker/config/WorkerConfigs.hpp"
#include <cstring>
#include <memory>

class TacoDecoderExtension : public IDecoderVendorExtension {
public:
    TacoConfig config;

    const char* kind() const noexcept override { return "taco"; }

    std::unique_ptr<IDecoderVendorExtension> clone() const override;

    bool validate(std::string& err) const override;

    double getChannelBytesPerPixel(int channel, void* priv_data, int pix_fmt) const override;

    int getOutputWidth(int channel = 0) const override;
    int getOutputHeight(int channel = 0) const override;

    bool applyToCodecContext(void* priv_data,
                             int source_width, int source_height) override;

    // ========================================
    // Taco 厂商专属映射（从 TacoConfigBuilder / FFmpegDecodeWorker 迁入）
    // ========================================

    /** OutputFormat → Taco PP RGB 驱动寄存器值（原 TacoConfigBuilder::mapEnumToRgbDriverValue） */
    static int mapOutputFormatToRgbDriverValue(OutputFormat format) {
        switch (format) {
            case OutputFormat::RGB_RGB888: return 1;
            case OutputFormat::RGB_RGB888_PLANAR: return 2;
            case OutputFormat::RGB_BGR888: return 3;
            case OutputFormat::RGB_BGR888_PLANAR: return 4;
            case OutputFormat::RGB_R16G16B16: return 5;
            case OutputFormat::RGB_B16G16R16: return 7;
            case OutputFormat::RGB_ARGB888: return 9;
            case OutputFormat::RGB_ABGR888: return 11;
            case OutputFormat::RGB_RGBA888: return 13;
            case OutputFormat::RGB_BGRA888: return 15;
            case OutputFormat::RGB_RGBX888: return -1;
            case OutputFormat::RGB_BGRX888: return -1;
            case OutputFormat::RGB_A2R10G10B10: return 17;
            case OutputFormat::RGB_A2B10G10R10: return 19;
            case OutputFormat::RGB_R10G10B10A2: return 21;
            case OutputFormat::RGB_B10G10R10A2: return 23;
            case OutputFormat::RGB_XRGB888: return 25;
            case OutputFormat::RGB_XBGR888: return 27;
            case OutputFormat::RGB_GBRP: return 28;
            default: return 9;
        }
    }

    /** Taco PP RGB 驱动寄存器值 → OutputFormat（原 FFmpegDecodeWorker::mapRgbDriverValueToEnum） */
    static OutputFormat mapRgbDriverValueToOutputFormat(int driver_value) {
        switch (driver_value) {
            case 1:  return OutputFormat::RGB_RGB888;
            case 2:  return OutputFormat::RGB_RGB888_PLANAR;
            case 3:  return OutputFormat::RGB_BGR888;
            case 4:  return OutputFormat::RGB_BGR888_PLANAR;
            case 5:  return OutputFormat::RGB_R16G16B16;
            case 7:  return OutputFormat::RGB_B16G16R16;
            case 9:  return OutputFormat::RGB_ARGB888;
            case 11: return OutputFormat::RGB_ABGR888;
            case 13: return OutputFormat::RGB_RGBA888;
            case 15: return OutputFormat::RGB_BGRA888;
            case 17: return OutputFormat::RGB_A2R10G10B10;
            case 19: return OutputFormat::RGB_A2B10G10R10;
            case 21: return OutputFormat::RGB_R10G10B10A2;
            case 23: return OutputFormat::RGB_B10G10R10A2;
            case 25: return OutputFormat::RGB_XRGB888;
            case 27: return OutputFormat::RGB_XBGR888;
            case 28: return OutputFormat::RGB_GBRP;
            default: return OutputFormat::RGB_ARGB888;
        }
    }

    /** OutputFormat → 每像素字节数（原 FFmpegDecodeWorker::getBytesPerPixelFromFormat） */
    static double getBytesPerPixelFromOutputFormat(OutputFormat format) {
        switch (format) {
            case OutputFormat::RGB_ARGB888:
            case OutputFormat::RGB_ABGR888:
            case OutputFormat::RGB_RGBA888:
            case OutputFormat::RGB_BGRA888:
            case OutputFormat::RGB_XRGB888:
            case OutputFormat::RGB_XBGR888:
            case OutputFormat::RGB_RGBX888:
            case OutputFormat::RGB_BGRX888:
                return 4.0;
            case OutputFormat::RGB_A2R10G10B10:
            case OutputFormat::RGB_A2B10G10R10:
            case OutputFormat::RGB_R10G10B10A2:
            case OutputFormat::RGB_B10G10R10A2:
                return 4.0;
            case OutputFormat::RGB_RGB888:
            case OutputFormat::RGB_BGR888:
            case OutputFormat::RGB_RGB888_PLANAR:
            case OutputFormat::RGB_BGR888_PLANAR:
            case OutputFormat::RGB_GBRP:
                return 3.0;
            case OutputFormat::RGB_R16G16B16:
            case OutputFormat::RGB_B16G16R16:
                return 6.0;
            default:
                return 4.0;
        }
    }
};

/// 从注册表或 useTaco 创建的 vendor 中取得 TACO 扩展；kind 不符返回 nullptr
TacoDecoderExtension* tacoVendorExtension(WorkerConfig::DecoderConfig& d);
const TacoDecoderExtension* tacoVendorExtension(const WorkerConfig::DecoderConfig& d);

TacoConfig* tacoDecoderConfig(WorkerConfig::DecoderConfig& d);
const TacoConfig* tacoDecoderConfig(const WorkerConfig::DecoderConfig& d);

std::unique_ptr<TacoDecoderExtension> makeTacoDecoderExtension(const TacoConfig& cfg);

/// 动态库导出符号（与 TacoDecoderPluginLoader / dlsym 配合）
extern "C" void register_taco_decoder_vendor();

#endif
