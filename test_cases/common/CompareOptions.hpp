/**
 * @file CompareOptions.hpp
 * @brief COMPARE 模式横切 CLI 选项（仿 DataSourceOptions）
 *
 * 统一注册 -p/-S、阈值、producer、compare-target，供 vdec/venc 复用。
 * ExecuteMode 不拥有这些参数；本助手只负责 CLI → WorkerConfig。
 *
 * 阈值短选项兼容：
 * - VdecStyle: -P/--min-psnr, -M/--min-ssim
 * - VencStyle: -M/--min-psnr, -N/--min-ssim
 */

#ifndef COMPARE_OPTIONS_HPP
#define COMPARE_OPTIONS_HPP

#include "consumptionline/config/ConsumerTypeConfigBuilder.hpp"
#include "productionline/worker/config/WorkerConfigs.hpp"
#include "third_party/CLI11.hpp"

#include <string>

namespace test {

inline ConsumerTypeConfig::CompareType::TargetKind parseCompareTargetKind(
    const std::string& s, bool* ok = nullptr)
{
    using TK = ConsumerTypeConfig::CompareType::TargetKind;
    auto set_ok = [&](bool v) { if (ok) *ok = v; };
    if (s.empty()) {
        set_ok(true);
        return TK::TARGET_UNSPECIFIED;
    }
    if (s == "peer" || s == "PEER" || s == "hw-sw" || s == "HW_SW") {
        set_ok(true);
        return TK::TARGET_PEER;
    }
    if (s == "source-ref" || s == "SOURCE_REF" || s == "source_ref" ||
        s == "enc" || s == "encode-quality") {
        set_ok(true);
        return TK::TARGET_SOURCE_REF;
    }
    set_ok(false);
    return TK::TARGET_UNSPECIFIED;
}

struct CompareOptions {
    enum class ThresholdStyle {
        Vdec,  ///< -P min-psnr, -M min-ssim
        Venc,  ///< -M min-psnr, -N min-ssim
    };

    bool enable_psnr = false;
    bool enable_ssim = false;
    double min_psnr = 0.0;   ///< 0 = CLI 未指定有效阈值
    double min_ssim = 0.0;
    std::string producer_type;
    std::string compare_target_str;

    void registerTo(CLI::App& app,
                    ThresholdStyle threshold_style,
                    double default_min_psnr = 0.0,
                    double default_min_ssim = 0.0) {
        min_psnr = default_min_psnr;
        min_ssim = default_min_ssim;

        app.add_flag("-p,--psnr", enable_psnr,
                     "启用 PSNR（打开 ExecuteMode::COMPARE）");
        app.add_flag("-S,--ssim", enable_ssim,
                     "启用 SSIM（打开 ExecuteMode::COMPARE）");

        if (threshold_style == ThresholdStyle::Vdec) {
            app.add_option("-P,--min-psnr", min_psnr, "PSNR 阈值 (dB)");
            app.add_option("-M,--min-ssim", min_ssim, "SSIM 阈值");
        } else {
            app.add_option("-M,--min-psnr", min_psnr,
                           "PSNR 阈值 (dB)，与 stress 脚本 -M 一致");
            app.add_option("-N,--min-ssim", min_ssim,
                           "SSIM 阈值，与 stress 脚本 -N 一致");
        }

        app.add_option("--mg-datasource-producer-type,--producer", producer_type,
            "COMPARE datasource 生产者类型；未设置则按驱动默认。"
            "可选: FFMPEG_PACKET_RECORDER|FFMPEG_DECODE|FFMPEG_ENCODE|"
            "FFMPEG_DECODE_THEN_ENCODE");

        app.add_option("--compare-target", compare_target_str,
            "COMPARE 对比目标：peer（hw↔sw）| source-ref（源↔编码→软解）；"
            "未设置则按驱动默认（vdec=peer，venc=source-ref）");
    }

    void applyCliToConfig(
        WorkerConfig& config,
        ConsumerTypeConfig::CompareType::TargetKind default_target) const
    {
        using TK = ConsumerTypeConfig::CompareType::TargetKind;

        auto compare_builder = CompareConfigBuilder(config.consumer_type.compare)
            .setEnablePsnr(enable_psnr)
            .setEnableSsim(enable_ssim);
        if (min_psnr > 0.0) {
            compare_builder.setMinPsnr(min_psnr);
        }
        if (min_ssim > 0.0) {
            compare_builder.setMinSsim(min_ssim);
        }

        bool parse_ok = true;
        TK target = parseCompareTargetKind(compare_target_str, &parse_ok);
        if (!parse_ok) {
            target = TK::TARGET_UNSPECIFIED;
        }
        if (target == TK::TARGET_UNSPECIFIED && (enable_psnr || enable_ssim)) {
            target = default_target;
        }
        compare_builder.setTargetKind(target);

        config.consumer_type = ConsumerTypeConfigBuilder(config.consumer_type)
            .setCompareConfig(compare_builder.build())
            .build();

        if (!producer_type.empty()) {
            config.mg_datasource_producer_type = producer_type;
        }
    }

    bool isCompareEnabled() const { return enable_psnr || enable_ssim; }
};

} // namespace test

#endif // COMPARE_OPTIONS_HPP
