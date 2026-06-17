#ifndef TACO_VENDOR_OPTIONS_HPP
#define TACO_VENDOR_OPTIONS_HPP

#include "vendor/contracts/IVendorOptionsRegistrar.hpp"
#include "vendor/taco/decode/TacoDecoderConfig.hpp"
#include "vendor/taco/decode/TacoDecoderExtension.hpp"
#include "productionline/worker/config/ConfigBuilders.hpp"
// 注意：使用方需在 include 本文件前先 include CLI11.hpp（如 VdecPlugin.cpp 已做）

/**
 * @brief Taco 厂商解码器 CLI 参数注册器
 *
 * 注册 Taco 硬件解码器 PP（后处理器）通道参数。
 * - ch0: YUV 格式输出通道
 * - ch1: RGB/YUV 格式输出通道（支持格式转换）
 * - ch0 + ch1 同时开启 = 双通道模式
 *
 * 使用示例：
 * @code
 * // 单通道（仅 ch0）
 * qa_cases vdec --rtsp <url> --psnr --vendor taco --ch0
 *
 * // 双通道（ch0 + ch1）
 * qa_cases vdec --rtsp <url> --psnr --vendor taco --ch0 --ch1
 * @endcode
 */
class TacoVendorOptions : public IVendorOptionsRegistrar {
public:
    const char* name() const noexcept override { return "taco"; }

    void registerTo(CLI::App& app) override {
        app.add_flag("--ch0", ch0_enable_,
            "Taco PP: 启用通道 0 输出 (YUV, 默认: true)");
        app.add_flag("--ch1", ch1_enable_,
            "Taco PP: 启用通道 1 输出 (RGB/YUV, 与 ch0 同时开启 = 双通道)");
    }

    std::unique_ptr<IDecoderVendorExtension> buildExtension() const override {
        auto cfg = TacoConfigBuilder()
            .setChannels(ch0_enable_, ch1_enable_)
            .build();
        return makeTacoDecoderExtension(cfg);
    }

private:
    bool ch0_enable_ = true;   ///< PP 通道 0，默认开启
    bool ch1_enable_ = false;  ///< PP 通道 1，默认关闭
};

#endif // TACO_VENDOR_OPTIONS_HPP
