/**
 * @file PPPlugin.cpp
 * @brief PPPlugin 实现
 * 
 * v6.0 重构：
 * - 消除 PPTestParams 中间结构体
 * - CLI 选项直接绑定到 config_ 成员
 * - 厂商分发表模式（与 DisplayPlugin 一致）
 * - 所有配置构建通过 Builder 完成
 */

#include "PPPlugin.hpp"
#include "../common/WorkerConfigFactory.hpp"
#include "consumptionline/core/BufferConsumerService.hpp"
#include "consumptionline/core/BufferConsumerStrategies.hpp"
#include "vendor/taco/decode/TacoDecoderExtension.hpp"

#include "../common/third_party/CLI11.hpp"

#include <iostream>
#include <sstream>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

namespace test {
namespace pp {

// ========================================
// 辅助函数
// ========================================

static std::vector<std::string> parseStringList(const std::string& str) {
    std::vector<std::string> result;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, ',')) {
        size_t start = item.find_first_not_of(" \t");
        size_t end = item.find_last_not_of(" \t");
        if (start != std::string::npos && end != std::string::npos) {
            result.push_back(item.substr(start, end - start + 1));
        } else if (!item.empty()) {
            result.push_back(item);
        }
    }
    return result;
}

static std::vector<int> parseChannels(const std::string& str) {
    std::vector<int> result;
    for (const auto& s : parseStringList(str)) {
        try {
            result.push_back(std::stoi(s));
        } catch (...) {}
    }
    return result.empty() ? std::vector<int>{0} : result;
}

static std::vector<OutputFormat> parseFormats(const std::string& str) {
    std::vector<OutputFormat> result;
    for (const auto& s : parseStringList(str)) {
        result.push_back(TacoConfigBuilder::mapFormatNameToEnum(s));
    }
    return result.empty() ? std::vector<OutputFormat>{OutputFormat::YUV_NV12} : result;
}

struct CropParams {
    int x = 0, y = 0, w = 0, h = 0;
};

static CropParams parseCrop(const std::string& str) {
    CropParams crop;
    if (str.empty()) return crop;
    int vals[4] = {0};
    int idx = 0;
    size_t start = 0, end;
    while ((end = str.find(',', start)) != std::string::npos && idx < 4) {
        vals[idx++] = std::stoi(str.substr(start, end - start));
        start = end + 1;
    }
    if (idx < 4 && start < str.length())
        vals[idx] = std::stoi(str.substr(start));
    crop.x = vals[0]; crop.y = vals[1]; crop.w = vals[2]; crop.h = vals[3];
    return crop;
}

static log4cplus::Logger& getLogger() {
    static log4cplus::Logger logger = log4cplus::Logger::getInstance(
        LOG4CPLUS_TEXT("components.test.pp"));
    return logger;
}

// ========================================
// 厂商分发表
// ========================================

const std::unordered_map<std::string, PPPlugin::DecoderBuilder>&
PPPlugin::vendorBuilders() {
    static const std::unordered_map<std::string, DecoderBuilder> map = {
        {"taco", &PPPlugin::buildTacoDecoder},
    };
    return map;
}

// ========================================
// TACO 厂商 DecoderConfig 构建
// ========================================

WorkerConfig::DecoderConfig PPPlugin::buildTacoDecoder() const {
    auto channels = channel_str_.empty() ? std::vector<int>{0} : parseChannels(channel_str_);
    auto formats = parseFormats(format_str_);
    auto color_std = TacoConfigBuilder::mapColorStdNameToEnum(color_std_str_);
    int w = pp_width_ > 0 ? pp_width_ : 1920;
    int h = pp_height_ > 0 ? pp_height_ : 1080;

    auto taco = TacoConfigBuilder();

    if (channels.size() >= 2) {
        taco.setChannels(true, true)
            .setOutputFormat(Channel::CH0, formats[0], color_std)
            .setOutputFormat(Channel::CH1,
                             formats.size() > 1 ? formats[1] : formats[0], color_std)
            .setScale(Channel::CH0, w, h)
            .setScale(Channel::CH1, w, h);
    } else if (channels[0] == 1) {
        taco.setChannels(false, true)
            .setOutputFormat(Channel::CH1, formats[0], color_std)
            .setScale(Channel::CH1, w, h);
    } else {
        taco.setChannels(true, false)
            .setOutputFormat(Channel::CH0, formats[0], color_std)
            .setScale(Channel::CH0, w, h);
    }

    auto crop = parseCrop(crop_str_);
    if (crop.w > 0 && crop.h > 0) {
        for (int c : channels) {
            taco.setCrop(c == 0 ? Channel::CH0 : Channel::CH1,
                         crop.x, crop.y, crop.w, crop.h);
        }
    }

    return DecoderConfigBuilder()
        .useTaco("h264", taco.build())
        .build();
}

// ========================================
// 预定义测试参数表（直接存 WorkerConfig）
// ========================================

const std::map<std::string, WorkerConfig>& PPPlugin::getPredefinedTests() {
    using F = common::WorkerConfigFactory;
    static std::map<std::string, WorkerConfig> tests = {
        // ════════════════════════════════════════════════════════════════════
        // PP0 YUV 格式（15 种，对应 lfl 分支 test_decode.cpp 定义）
        // ════════════════════════════════════════════════════════════════════
        {"pp0_yuv400_p010",       F::createPP0YuvConfig("", OutputFormat::YUV_P010, 1920, 1080, TacoColorSpace::BT2020_FULL)},
        {"pp0_yuv400_i010",       F::createPP0YuvConfig("", OutputFormat::YUV_P010, 1920, 1080, TacoColorSpace::BT2020_FULL)},
        {"pp0_yuv400_l010",       F::createPP0YuvConfig("", OutputFormat::YUV_P010, 1920, 1080, TacoColorSpace::BT2020_FULL)},
        {"pp0_yuv400_pack10",     F::createPP0YuvConfig("", OutputFormat::YUV_P010, 1920, 1080, TacoColorSpace::BT2020_FULL)},
        {"pp0_yuv400_8bit",       F::createPP0YuvConfig("", OutputFormat::YUV_NV12, 1920, 1080, TacoColorSpace::BT601_FULL)},
        {"pp0_yuv420_nv12_p010",  F::createPP0YuvConfig("", OutputFormat::YUV_P010, 1920, 1080, TacoColorSpace::BT2020_FULL)},
        {"pp0_yuv420_nv12_i010",  F::createPP0YuvConfig("", OutputFormat::YUV_P010, 1920, 1080, TacoColorSpace::BT2020_FULL)},
        {"pp0_yuv420_nv12_l010",  F::createPP0YuvConfig("", OutputFormat::YUV_P010, 1920, 1080, TacoColorSpace::BT2020_FULL)},
        {"pp0_yuv420_nv12_pack10",F::createPP0YuvConfig("", OutputFormat::YUV_P010, 1920, 1080, TacoColorSpace::BT2020_FULL)},
        {"pp0_yuv420_8bit_nv12",  F::createPP0YuvConfig("", OutputFormat::YUV_NV12, 1920, 1080, TacoColorSpace::BT601_FULL)},
        {"pp0_yuv420_nv21_p010_tiled", F::createPP0YuvConfig("", OutputFormat::YUV_P010, 1920, 1080, TacoColorSpace::BT2020_FULL)},
        {"pp0_yuv420_nv21_i011",  F::createPP0YuvConfig("", OutputFormat::YUV_P010, 1920, 1080, TacoColorSpace::BT2020_FULL)},
        {"pp0_yuv420_nv21_l010",  F::createPP0YuvConfig("", OutputFormat::YUV_P010, 1920, 1080, TacoColorSpace::BT2020_FULL)},
        {"pp0_yuv420_p010",       F::createPP0YuvConfig("", OutputFormat::YUV_P010, 1920, 1080, TacoColorSpace::BT2020_FULL)},
        {"pp0_yuv420_8bit_nv21",  F::createPP0YuvConfig("", OutputFormat::YUV_NV21, 1920, 1080, TacoColorSpace::BT601_FULL)},

        // 便捷别名
        {"pp0_nv12",              F::createPP0YuvConfig("", OutputFormat::YUV_NV12, 1920, 1080)},
        {"pp0_nv21",              F::createPP0YuvConfig("", OutputFormat::YUV_NV21, 1920, 1080)},
        {"pp0_i420",              F::createPP0YuvConfig("", OutputFormat::YUV_I420, 1920, 1080)},
        {"pp0_yv12",              F::createPP0YuvConfig("", OutputFormat::YUV_YV12, 1920, 1080)},
        {"pp0_p010",              F::createPP0YuvConfig("", OutputFormat::YUV_P010, 1920, 1080)},
        {"pp0_nv16",              F::createPP0YuvConfig("", OutputFormat::YUV_NV16, 1920, 1080)},
        {"pp0_nv61",              F::createPP0YuvConfig("", OutputFormat::YUV_NV61, 1920, 1080)},
        {"pp0_i422",              F::createPP0YuvConfig("", OutputFormat::YUV_I422, 1920, 1080)},
        {"pp0_nv24",              F::createPP0YuvConfig("", OutputFormat::YUV_NV24, 1920, 1080)},
        {"pp0_i444",              F::createPP0YuvConfig("", OutputFormat::YUV_I444, 1920, 1080)},

        // ════════════════════════════════════════════════════════════════════
        // PP1 RGB 格式（18 种）
        // ════════════════════════════════════════════════════════════════════
        {"pp1_argb2101010",       F::createPP1RgbConfig("", OutputFormat::RGB_A2R10G10B10, 1920, 1080, TacoColorSpace::BT601_FULL)},
        {"pp1_abgr2101010",       F::createPP1RgbConfig("", OutputFormat::RGB_A2B10G10R10, 1920, 1080, TacoColorSpace::BT601_FULL)},
        {"pp1_bgra2101010",       F::createPP1RgbConfig("", OutputFormat::RGB_B10G10R10A2, 1920, 1080, TacoColorSpace::BT601_FULL)},
        {"pp1_rgba2101010",       F::createPP1RgbConfig("", OutputFormat::RGB_R10G10B10A2, 1920, 1080, TacoColorSpace::BT601_FULL)},
        {"pp1_abgr8888",          F::createPP1RgbConfig("", OutputFormat::RGB_ABGR888, 1920, 1080, TacoColorSpace::BT601_FULL)},
        {"pp1_argb8888",          F::createPP1RgbConfig("", OutputFormat::RGB_ARGB888, 1920, 1080, TacoColorSpace::BT601_FULL)},
        {"pp1_bgr888",            F::createPP1RgbConfig("", OutputFormat::RGB_BGR888, 1920, 1080, TacoColorSpace::BT601_FULL)},
        {"pp1_bgra8888",          F::createPP1RgbConfig("", OutputFormat::RGB_BGRA888, 1920, 1080, TacoColorSpace::BT601_FULL)},
        {"pp1_bgrx8888",          F::createPP1RgbConfig("", OutputFormat::RGB_BGRX888, 1920, 1080, TacoColorSpace::BT601_FULL)},
        {"pp1_rgb888",            F::createPP1RgbConfig("", OutputFormat::RGB_RGB888, 1920, 1080, TacoColorSpace::BT601_FULL)},
        {"pp1_rgba8888",          F::createPP1RgbConfig("", OutputFormat::RGB_RGBA888, 1920, 1080, TacoColorSpace::BT601_FULL)},
        {"pp1_rgbx8888",          F::createPP1RgbConfig("", OutputFormat::RGB_RGBX888, 1920, 1080, TacoColorSpace::BT601_FULL)},
        {"pp1_xrgb8888",          F::createPP1RgbConfig("", OutputFormat::RGB_XRGB888, 1920, 1080, TacoColorSpace::BT601_FULL)},
        {"pp1_xbgr8888",          F::createPP1RgbConfig("", OutputFormat::RGB_XBGR888, 1920, 1080, TacoColorSpace::BT601_FULL)},
        {"pp1_rgb888_planar",     F::createPP1RgbConfig("", OutputFormat::RGB_RGB888_PLANAR, 1920, 1080, TacoColorSpace::BT601_FULL)},
        {"pp1_bgr888_planar",     F::createPP1RgbConfig("", OutputFormat::RGB_BGR888_PLANAR, 1920, 1080, TacoColorSpace::BT601_FULL)},
        {"pp1_rgb161616",         F::createPP1RgbConfig("", OutputFormat::RGB_R16G16B16, 1920, 1080, TacoColorSpace::BT601_FULL)},
        {"pp1_bgr161616",         F::createPP1RgbConfig("", OutputFormat::RGB_B16G16R16, 1920, 1080, TacoColorSpace::BT601_FULL)},
        {"pp1_rgb161616_planar",  F::createPP1RgbConfig("", OutputFormat::RGB_GBRP, 1920, 1080, TacoColorSpace::BT601_FULL)},

        // PP1 RGB 便捷别名
        {"pp1_argb888",           F::createPP1RgbConfig("", OutputFormat::RGB_ARGB888, 1920, 1080)},
        {"pp1_abgr888",           F::createPP1RgbConfig("", OutputFormat::RGB_ABGR888, 1920, 1080)},
        {"pp1_rgba888",           F::createPP1RgbConfig("", OutputFormat::RGB_RGBA888, 1920, 1080)},
        {"pp1_bgra888",           F::createPP1RgbConfig("", OutputFormat::RGB_BGRA888, 1920, 1080)},
        {"pp1_r16g16b16",         F::createPP1RgbConfig("", OutputFormat::RGB_R16G16B16, 1920, 1080)},
        {"pp1_b16g16r16",         F::createPP1RgbConfig("", OutputFormat::RGB_B16G16R16, 1920, 1080)},
        {"pp1_gbrp",              F::createPP1RgbConfig("", OutputFormat::RGB_GBRP, 1920, 1080)},

        // ════════════════════════════════════════════════════════════════════
        // PP1 YUV 格式（16 种）
        // ════════════════════════════════════════════════════════════════════
        {"pp1_yuv400_p010",       F::createPP1YuvConfig("", OutputFormat::YUV_P010, 1920, 1080, TacoColorSpace::BT2020_FULL)},
        {"pp1_yuv400_i010",       F::createPP1YuvConfig("", OutputFormat::YUV_P010, 1920, 1080, TacoColorSpace::BT2020_FULL)},
        {"pp1_yuv400_l010",       F::createPP1YuvConfig("", OutputFormat::YUV_P010, 1920, 1080, TacoColorSpace::BT2020_FULL)},
        {"pp1_yuv400_pack10",     F::createPP1YuvConfig("", OutputFormat::YUV_P010, 1920, 1080, TacoColorSpace::BT2020_FULL)},
        {"pp1_yuv400_8bit",       F::createPP1YuvConfig("", OutputFormat::YUV_NV12, 1920, 1080, TacoColorSpace::BT601_FULL)},
        {"pp1_yuv420_nv12_p010",  F::createPP1YuvConfig("", OutputFormat::YUV_P010, 1920, 1080, TacoColorSpace::BT2020_FULL)},
        {"pp1_yuv420_nv12_i010",  F::createPP1YuvConfig("", OutputFormat::YUV_P010, 1920, 1080, TacoColorSpace::BT2020_FULL)},
        {"pp1_yuv420_nv12_l010",  F::createPP1YuvConfig("", OutputFormat::YUV_P010, 1920, 1080, TacoColorSpace::BT2020_FULL)},
        {"pp1_yuv420_nv12_pack10",F::createPP1YuvConfig("", OutputFormat::YUV_P010, 1920, 1080, TacoColorSpace::BT2020_FULL)},
        {"pp1_yuv420_8bit_nv12",  F::createPP1YuvConfig("", OutputFormat::YUV_NV12, 1920, 1080, TacoColorSpace::BT601_FULL)},
        {"pp1_yuv420_nv21_p010_tiled", F::createPP1YuvConfig("", OutputFormat::YUV_P010, 1920, 1080, TacoColorSpace::BT2020_FULL)},
        {"pp1_yuv420_nv21_i011",  F::createPP1YuvConfig("", OutputFormat::YUV_P010, 1920, 1080, TacoColorSpace::BT2020_FULL)},
        {"pp1_yuv420_nv21_l010",  F::createPP1YuvConfig("", OutputFormat::YUV_P010, 1920, 1080, TacoColorSpace::BT2020_FULL)},
        {"pp1_yuv420_p010",       F::createPP1YuvConfig("", OutputFormat::YUV_P010, 1920, 1080, TacoColorSpace::BT2020_FULL)},
        {"pp1_yuv420_8bit_nv21",  F::createPP1YuvConfig("", OutputFormat::YUV_NV21, 1920, 1080, TacoColorSpace::BT601_FULL)},
        {"pp1_yuv420_nv21_p010",  F::createPP1YuvConfig("", OutputFormat::YUV_P010, 1920, 1080, TacoColorSpace::BT2020_FULL)},

        // PP1 YUV 便捷别名
        {"pp1_nv12",              F::createPP1YuvConfig("", OutputFormat::YUV_NV12, 1920, 1080)},
        {"pp1_nv21",              F::createPP1YuvConfig("", OutputFormat::YUV_NV21, 1920, 1080)},
        {"pp1_i420",              F::createPP1YuvConfig("", OutputFormat::YUV_I420, 1920, 1080)},
        {"pp1_yv12",              F::createPP1YuvConfig("", OutputFormat::YUV_YV12, 1920, 1080)},
        {"pp1_p010",              F::createPP1YuvConfig("", OutputFormat::YUV_P010, 1920, 1080)},
        {"pp1_nv16",              F::createPP1YuvConfig("", OutputFormat::YUV_NV16, 1920, 1080)},
        {"pp1_nv61",              F::createPP1YuvConfig("", OutputFormat::YUV_NV61, 1920, 1080)},
        {"pp1_i422",              F::createPP1YuvConfig("", OutputFormat::YUV_I422, 1920, 1080)},
        {"pp1_nv24",              F::createPP1YuvConfig("", OutputFormat::YUV_NV24, 1920, 1080)},
        {"pp1_i444",              F::createPP1YuvConfig("", OutputFormat::YUV_I444, 1920, 1080)},

        // ════════════════════════════════════════════════════════════════════
        // Multi-PP 测试（10 个）
        // ════════════════════════════════════════════════════════════════════
        {"multi_pp_t01",    F::createMultiPPConfig("", OutputFormat::YUV_NV12, OutputFormat::RGB_RGB888, 1920, 1080)},
        {"multi_pp_t02",    F::createMultiPPConfig("", OutputFormat::YUV_NV12, OutputFormat::RGB_ARGB888, 1920, 1080)},
        {"multi_pp_t03",    F::createMultiPPConfig("", OutputFormat::YUV_NV21, OutputFormat::RGB_BGR888, 1920, 1080)},
        {"multi_pp_t04",    F::createMultiPPConfig("", OutputFormat::YUV_NV12, OutputFormat::RGB_RGB888, 1920, 1080)},
        {"multi_pp_t05",    F::createMultiPPConfig("", OutputFormat::YUV_P010, OutputFormat::RGB_R16G16B16, 1920, 1080)},
        {"multi_pp_t06",    F::createMultiPPConfig("", OutputFormat::YUV_P010, OutputFormat::RGB_R16G16B16, 1920, 1080)},
        {"multi_pp_t07",    F::createMultiPPConfig("", OutputFormat::YUV_NV12, OutputFormat::RGB_RGB888, 1920, 1080)},
        {"multi_pp_t09",    F::createMultiPPConfig("", OutputFormat::YUV_NV12, OutputFormat::RGB_RGB888_PLANAR, 1920, 1080)},
        {"multi_pp_t10",    F::createMultiPPConfig("", OutputFormat::YUV_P010, OutputFormat::RGB_ARGB888, 1920, 1080)},
        {"multi_pp_t11",    F::createMultiPPConfig("", OutputFormat::YUV_NV12, OutputFormat::YUV_NV21, 1920, 1080)},

        // ════════════════════════════════════════════════════════════════════
        // Multi-PP Crop/Scale 测试（8 个）
        // ════════════════════════════════════════════════════════════════════
        {"multi_pp_crop1",  F::createCropConfig("", 0, 0, 4096, 2160, 1920, 1080)},
        {"multi_pp_crop2",  F::createCropConfig("", 0, 0, 32768, 32768, 1280, 720)},
        {"multi_pp_crop3",  F::createCropConfig("", 0, 0, 4096, 2160, 1920, 1080)},
        {"multi_pp_crop4",  F::createCropConfig("", 0, 0, 32768, 32768, 1280, 720)},
        {"multi_pp_scale1", F::createPP0YuvConfig("", OutputFormat::YUV_NV12, 256, 256, TacoColorSpace::BT709_FULL)},
        {"multi_pp_scale2", F::createPP1YuvConfig("", OutputFormat::YUV_NV12, 128, 128, TacoColorSpace::BT709_FULL)},
        {"multi_pp_scale3", F::createMultiPPConfig("", OutputFormat::YUV_NV12, OutputFormat::YUV_NV12, 256, 256, TacoColorSpace::BT709_FULL)},
        {"multi_pp_scale4", F::createMultiPPConfig("", OutputFormat::YUV_NV12, OutputFormat::YUV_NV12, 128, 128, TacoColorSpace::BT709_FULL)},

        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - PP Crop 测试（带分辨率）
        // ════════════════════════════════════════════════════════════════════
        {"pp0_720p_crop",           F::createCropConfig("", 0, 0, 1280, 720, 1280, 720)},
        {"pp0_1080p_crop",          F::createCropConfig("", 0, 0, 1920, 1080, 1920, 1080)},
        {"pp1_720p_rgb_crop",       F::createPP1RgbConfig("", OutputFormat::RGB_RGB888, 1280, 720, TacoColorSpace::BT709_FULL)},
        {"pp1_1080p_rgb_crop",      F::createPP1RgbConfig("", OutputFormat::RGB_RGB888, 1920, 1080, TacoColorSpace::BT709_FULL)},
        {"pp1_720p_yuv_crop",       F::createPP1YuvConfig("", OutputFormat::YUV_NV12, 1280, 720, TacoColorSpace::BT709_FULL)},
        {"pp1_1080p_yuv_crop",      F::createPP1YuvConfig("", OutputFormat::YUV_NV12, 1920, 1080, TacoColorSpace::BT709_FULL)},

        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - Multi-PP 扩展测试
        // ════════════════════════════════════════════════════════════════════
        {"multi_pp",                F::createMultiPPConfig("", OutputFormat::YUV_NV12, OutputFormat::RGB_RGB888, 1920, 1080)},
        {"multi_pp_crop",           F::createMultiPPConfig("", OutputFormat::YUV_NV12, OutputFormat::RGB_RGB888, 1920, 1080, TacoColorSpace::BT709_FULL)},
        {"multi_pp_scale",          F::createMultiPPConfig("", OutputFormat::YUV_NV12, OutputFormat::RGB_RGB888, 960, 540, TacoColorSpace::BT709_FULL)},
        {"multi_pp_crop_scale",     F::createMultiPPConfig("", OutputFormat::YUV_NV12, OutputFormat::RGB_RGB888, 960, 540, TacoColorSpace::BT709_FULL)},

        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - PP0 带分辨率的格式测试
        // ════════════════════════════════════════════════════════════════════
        {"pp0_720p_nv12",           F::createPP0YuvConfig("", OutputFormat::YUV_NV12, 1280, 720)},
        {"pp0_720p_p010",           F::createPP0YuvConfig("", OutputFormat::YUV_P010, 1280, 720)},
        {"pp0_1080p_nv21",          F::createPP0YuvConfig("", OutputFormat::YUV_NV21, 1920, 1080)},
        {"pp0_4k_nv12",             F::createPP0YuvConfig("", OutputFormat::YUV_NV12, 3840, 2160)},

        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - PP1 带分辨率的格式测试
        // ════════════════════════════════════════════════════════════════════
        {"pp1_720p_argb8888",       F::createPP1RgbConfig("", OutputFormat::RGB_ARGB888, 1280, 720)},
        {"pp1_4k_argb8888",         F::createPP1RgbConfig("", OutputFormat::RGB_ARGB888, 3840, 2160)},
        {"pp1_720p_rgb888",         F::createPP1RgbConfig("", OutputFormat::RGB_RGB888, 1280, 720)},
        {"pp1_1080p_argb8888",      F::createPP1RgbConfig("", OutputFormat::RGB_ARGB888, 1920, 1080)},

        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - Crop 带输出分辨率
        // ════════════════════════════════════════════════════════════════════
        {"crop_720p_1024x576",      F::createCropConfig("", 0, 0, 1280, 720, 1024, 576)},
        {"crop_1080p_1600x900",     F::createCropConfig("", 0, 0, 1920, 1080, 1600, 900)},

        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - Scale 带输出分辨率
        // ════════════════════════════════════════════════════════════════════
        {"scale_720p_512x288",      F::createPP0YuvConfig("", OutputFormat::YUV_NV12, 512, 288, TacoColorSpace::BT709_FULL)},
        {"scale_1080p_800x450",     F::createPP0YuvConfig("", OutputFormat::YUV_NV12, 800, 450, TacoColorSpace::BT709_FULL)},

        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - H265 PP0/PP1 格式测试
        // ════════════════════════════════════════════════════════════════════
        {"pp0_h265_720p_nv12",      F::createPP0YuvConfig("", OutputFormat::YUV_NV12, 1280, 720)},
        {"pp0_h265_1080p_p010",     F::createPP0YuvConfig("", OutputFormat::YUV_P010, 1920, 1080)},
        {"pp0_h265_4k_nv12",        F::createPP0YuvConfig("", OutputFormat::YUV_NV12, 3840, 2160)},
        {"pp1_h265_720p_rgb888",    F::createPP1RgbConfig("", OutputFormat::RGB_RGB888, 1280, 720)},
        {"pp1_h265_1080p_argb8888", F::createPP1RgbConfig("", OutputFormat::RGB_ARGB888, 1920, 1080)},

        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - H265 Crop+Scale
        // ════════════════════════════════════════════════════════════════════
        {"h265_1080p_crop_scale",   F::createCropConfig("", 0, 0, 1920, 1080, 960, 540)},
    };
    return tests;
}

// ========================================
// IOptionPlugin 接口实现
// ========================================

void PPPlugin::registerOptions(CLI::App& app) {
    // ── 厂商选择 ──
    app.add_option("--vendor", vendor_str_, "PP 厂商: taco(默认)");

    // ── 直接绑定到 config_.data_source ──
    app.add_option("-i,--input", config_.data_source.path, "输入视频路径");
    app.add_option("-m,--max-frames", config_.data_source.max_frames,
                   "最大帧数（数据源读取与消费循环共用，0=不覆盖）");

    // ── 直接绑定到 config_.decoder ──
    app.add_flag("!--software", config_.decoder.enable_hardware,
                 "使用软件解码（默认硬件）");

    // ── PP 输出分辨率（供各厂商 Builder 使用）──
    app.add_option("-W,--width", pp_width_, "PP 输出宽度");
    app.add_option("-H,--height", pp_height_, "PP 输出高度");

    // ── 直接绑定到 config_.consumer_type.compare ──
    app.add_flag("-p,--psnr", config_.consumer_type.compare.enable_psnr, "启用 PSNR 验证");
    app.add_flag("-S,--ssim", config_.consumer_type.compare.enable_ssim, "启用 SSIM 验证");
    app.add_option("-P,--min-psnr", config_.consumer_type.compare.min_psnr, "PSNR 阈值");
    app.add_option("-M,--min-ssim", config_.consumer_type.compare.min_ssim, "SSIM 阈值");

    // ── 直接绑定到 config_.consumer_type ──
    app.add_flag("-v,--verbose", config_.consumer_type.verbose, "详细日志");

    // ── 直接绑定到 config_.consumer_type.save_raw ──
    app.add_option("-o,--output", config_.consumer_type.save_raw.output_paths, "输出文件路径")
        ->delimiter(',');
    app.add_option("-n,--save", config_.consumer_type.save_raw.max_frames_per_channel, "保存帧数")
        ->delimiter(',');

    // ── 厂商无关的 PP 概念参数（字符串中间变量，目标在 vendor 指针后面）──
    app.add_option("-f,--format", format_str_, "输出格式, 多通道用逗号分隔");
    app.add_option("-c,--channel", channel_str_, "通道选择 (0|1|0,1)");
    app.add_option("-C,--crop", crop_str_, "裁剪区域 (x,y,w,h)");
    app.add_option("-s,--color-std", color_std_str_, "颜色标准 (bt601|bt709|bt2020)");

    // ── 便捷入口 ──
    app.add_option("-R,--resolution", resolution_str_, "分辨率 (如 1920x1080)");

    // ── 控制 ──
    app.add_flag("-l,--list", show_list_, "列出所有预定义测试");
    ds_opts_.registerTo(app);
    app.add_option("positional", positional_args_, "测试名或输入文件路径");

    app.footer(
        "Examples:\n"
        "  qa_cases pp video.mp4\n"
        "  qa_cases pp --channel 0 --format nv12 video.mp4\n"
        "  qa_cases pp --channel 1 --format argb888 video.mp4\n"
        "  qa_cases pp --psnr video.mp4\n"
        "  qa_cases pp pp1_argb888 video.mp4\n"
        "  qa_cases pp --vendor taco video.mp4\n"
    );
}

void PPPlugin::applyTo(WorkerConfig& config) const {
    // ConsumerTypeConfigBuilder（seed 模式：从 shared config 出发，叠加 CLI 选项）
    auto ct_builder = ConsumerTypeConfigBuilder(config.consumer_type);

    if (config_.consumer_type.compare.enable_psnr || config_.consumer_type.compare.enable_ssim) {
        ct_builder.setCompareConfig(
            CompareConfigBuilder(config.consumer_type.compare)
                .setEnablePsnr(true)
                .setEnableSsim(true)
                .setMinPsnr(config_.consumer_type.compare.min_psnr)
                .setMinSsim(config_.consumer_type.compare.min_ssim)
                .build());
    }
    if (config_.consumer_type.verbose) {
        ct_builder.setVerbose(true);
    }
    if (!config_.consumer_type.save_raw.output_paths.empty()) {
        std::vector<int> mfc = config_.consumer_type.save_raw.max_frames_per_channel;
        if (mfc.size() != config_.consumer_type.save_raw.output_paths.size()) {
            mfc.assign(config_.consumer_type.save_raw.output_paths.size(), -1);
        }
        ct_builder.setSaveRawConfig(
            SaveRawConfigBuilder(config.consumer_type.save_raw)
                .setEnable(true)
                .setOutputPaths(config_.consumer_type.save_raw.output_paths)
                .setMaxFramesPerChannel(mfc)
                .build());
    }
    if (config_.data_source.max_frames != -1) {
        ct_builder.setMaxFrames(config_.data_source.max_frames);
    }

    config.consumer_type = ct_builder.build();

    // DataSourceConfigBuilder（seed 模式）
    config.data_source = DataSourceConfigBuilder(config.data_source)
        .setPathIfNonEmpty(config_.data_source.path)
        .setMaxFramesIfNonZero(config_.data_source.max_frames)
        .build();

    ds_opts_.applyTo(config);
}

int PPPlugin::handlePreActions() {
    // resolution_str_ 便捷写法 → pp_width_ / pp_height_
    if (!resolution_str_.empty()) {
        size_t pos = resolution_str_.find('x');
        if (pos != std::string::npos) {
            pp_width_  = std::stoi(resolution_str_.substr(0, pos));
            pp_height_ = std::stoi(resolution_str_.substr(pos + 1));
        }
    }

    // positional 参数处理
    for (const auto& a : positional_args_) {
        const auto& tests = getPredefinedTests();
        auto it = tests.find(a);
        if (it != tests.end()) {
            // 预定义测试命中：用 WorkerConfigBuilder 合并 preset 与 CLI 覆盖
            const WorkerConfig& preset = it->second;
            config_ = WorkerConfigBuilder()
                .setDataSourceConfig(
                    DataSourceConfigBuilder(preset.data_source)
                        .setPathIfNonEmpty(config_.data_source.path)
                        .setMaxFramesIfNonZero(config_.data_source.max_frames)
                        .build()
                )
                .setDecoderConfig(preset.decoder)
                .setEncoderConfig(preset.encoder)
                .setConsumerTypeConfig(
                    ConsumerTypeConfigBuilder(config_.consumer_type)
                        .build()
                )
                .setGlobalConfig(preset.global)
                .build();

            // 预设中的 PP 分辨率回写到 pp_width_/pp_height_（与 CLI 传参等价）
            // 修复：若 CLI 未显式指定 -W/-H，则从预设的 TacoConfig 中读取，
            // 避免 buildTacoDecoder() 因 pp_width_=0 回退到默认的 1920x1080
            if (pp_width_ <= 0 || pp_height_ <= 0) {
                auto* taco = tacoDecoderConfig(config_.decoder);
                if (taco) {
                    if (pp_width_ <= 0 && taco->ch0_scale_width > 0)
                        pp_width_ = taco->ch0_scale_width;
                    if (pp_height_ <= 0 && taco->ch0_scale_height > 0)
                        pp_height_ = taco->ch0_scale_height;
                }
            }
            continue;
        }
        if (config_.data_source.path.empty()) {
            config_.data_source = DataSourceConfigBuilder(config_.data_source)
                .setPath(a)
                .build();
        }
    }

    if (show_list_) { listTests(); return 0; }
    if (config_.data_source.path.empty()) {
        LOG4CPLUS_ERROR(getLogger(), "No input file specified");
        return 1;
    }
    return -1;
}

std::string PPPlugin::getTestName() const {
    auto channels = channel_str_.empty() ? std::vector<int>{0} : parseChannels(channel_str_);
    auto formats = parseFormats(format_str_);
    int w = pp_width_ > 0 ? pp_width_ : 1920;
    int h = pp_height_ > 0 ? pp_height_ : 1080;

    std::ostringstream test_name;
    for (size_t i = 0; i < channels.size(); ++i) {
        if (i > 0) test_name << ",";
        test_name << channels[i];
    }
    for (size_t i = 0; i < formats.size(); ++i) {
        test_name << (i == 0 ? " " : ",")
                  << TacoConfigBuilder::mapFormatEnumToName(formats[i]);
    }
    test_name << " (" << w << "x" << h << ")";
    return test_name.str();
}

std::vector<WorkerConfig> PPPlugin::buildPipelineConfigs(const WorkerConfig& shared_config) {
    if (config_.data_source.path.empty()) return {};

    auto sync_data_source = [](const WorkerConfig& shared) {
        return DataSourceConfigBuilder(shared.data_source)
            .setMaxFrames(shared.data_source.max_frames)
            .setLoop(shared.data_source.loop)
            .build();
    };

    // 分发表：根据 vendor_str_ 选择厂商 builder
    const auto& builders = vendorBuilders();
    auto it = builders.find(vendor_str_);
    if (it == builders.end()) {
        LOG4CPLUS_ERROR(getLogger(), "Unknown PP vendor: " + vendor_str_);
        return {};
    }

    // 软件解码模式
    if (!config_.decoder.enable_hardware) {
        auto sw_config = WorkerConfigBuilder()
            .setDataSourceConfig(
                DataSourceConfigBuilder(sync_data_source(shared_config))
                    .setPath(config_.data_source.path)
                    .build()
            )
            .setDecoderConfig(DecoderConfigBuilder().useSoftware().build())
            .setConsumerTypeConfig(shared_config.consumer_type)
            .setGlobalConfig(
                WorkerGlobalConfigBuilder().setWorkerType(WorkerType::FFMPEG_DECODE).build()
            )
            .build();
        LOG4CPLUS_WARN(getLogger(),
            "Software decode mode: Hardware PP features are not available");
        return {sw_config};
    }

    // 厂商分发表构建 DecoderConfig
    auto decoder = (this->*(it->second))();

    auto data_source = DataSourceConfigBuilder(sync_data_source(shared_config))
        .setPath(config_.data_source.path)
        .build();

    auto full_config = WorkerConfigBuilder()
        .setDataSourceConfig(data_source)
        .setDecoderConfig(decoder)
        .setConsumerTypeConfig(shared_config.consumer_type)
        .setGlobalConfig(
            WorkerGlobalConfigBuilder().setWorkerType(WorkerType::FFMPEG_DECODE).build()
        )
        .build();

    // COMPARE：多通道 → 通道比较
    auto channels = channel_str_.empty() ? std::vector<int>{0} : parseChannels(channel_str_);
    if ((shared_config.consumer_type.compare.enable_psnr ||
         shared_config.consumer_type.compare.enable_ssim)
        && channels.size() >= 2) {
        full_config.consumer_type = ConsumerTypeConfigBuilder(full_config.consumer_type)
            .setCompareConfig(
                CompareConfigBuilder(full_config.consumer_type.compare)
                    .setEnablePsnr(true)
                    .setEnableSsim(true)
                    .setMinPsnr(full_config.consumer_type.compare.min_psnr)
                    .setMinSsim(full_config.consumer_type.compare.min_ssim)
                    .build())
            .build();
        return {full_config};
    }

    // COMPARE：单通道 → HW vs SW
    // HW 解码器双通道输出（ch0 NV12 + ch1 RGB888）会与 SW 单通道 PTS 不匹配，
    // 此处禁用 ch1 使其仅单通道输出，与 SW 解码器对齐
    if (shared_config.consumer_type.compare.enable_psnr ||
        shared_config.consumer_type.compare.enable_ssim) {
        // HW 解码器改为单通道（禁用 ch1），与 SW 的 1 帧/帧对齐
        auto hw_decoder = full_config.decoder;
        auto* taco_cfg = tacoDecoderConfig(hw_decoder);
        if (taco_cfg) {
            taco_cfg->ch1_enable = false;
        }
        auto hw_config = WorkerConfigBuilder()
            .setDataSourceConfig(data_source)
            .setDecoderConfig(hw_decoder)
            .setConsumerTypeConfig(shared_config.consumer_type)
            .setGlobalConfig(
                WorkerGlobalConfigBuilder().setWorkerType(WorkerType::FFMPEG_DECODE).build()
            )
            .build();
        auto sw_config = WorkerConfigBuilder()
            .setDataSourceConfig(data_source)
            .setDecoderConfig(DecoderConfigBuilder().useSoftware().build())
            .setConsumerTypeConfig(shared_config.consumer_type)
            .setGlobalConfig(
                WorkerGlobalConfigBuilder().setWorkerType(WorkerType::FFMPEG_DECODE).build()
            )
            .build();
        return {hw_config, sw_config};
    }

    // SINGLE
    return {full_config};
}

// ========================================
// listTests
// ========================================

void PPPlugin::listTests() const {
    std::cout << "\nAvailable PP tests:\n";
    std::cout << "════════════════════════════════════════════════════════\n";

    std::cout << "\nPP0 YUV Format Tests (15 种 + 10 别名):\n";
    std::cout << "  YUV400 系列 (灰度):\n";
    std::cout << "    pp0_yuv400_p010       YUV400 P010 (10-bit grayscale)\n";
    std::cout << "    pp0_yuv400_i010       YUV400 I010 (10-bit grayscale)\n";
    std::cout << "    pp0_yuv400_l010       YUV400 L010 (10-bit grayscale)\n";
    std::cout << "    pp0_yuv400_pack10     YUV400 Pack10 (10-bit grayscale)\n";
    std::cout << "    pp0_yuv400_8bit       YUV400 8-bit (8-bit grayscale)\n";
    std::cout << "  YUV420 NV12 系列:\n";
    std::cout << "    pp0_yuv420_nv12_p010  YUV420 NV12 P010 (10-bit)\n";
    std::cout << "    pp0_yuv420_nv12_i010  YUV420 NV12 I010 (10-bit)\n";
    std::cout << "    pp0_yuv420_nv12_l010  YUV420 NV12 L010 (10-bit)\n";
    std::cout << "    pp0_yuv420_nv12_pack10 YUV420 NV12 Pack10 (10-bit)\n";
    std::cout << "    pp0_yuv420_8bit_nv12  YUV420 8-bit NV12\n";
    std::cout << "  YUV420 NV21 系列:\n";
    std::cout << "    pp0_yuv420_nv21_p010_tiled YUV420 NV21 P010 Tiled-4x4\n";
    std::cout << "    pp0_yuv420_nv21_i011  YUV420 NV21 I011\n";
    std::cout << "    pp0_yuv420_nv21_l010  YUV420 NV21 L010\n";
    std::cout << "    pp0_yuv420_p010       YUV420 P010 (10-bit)\n";
    std::cout << "    pp0_yuv420_8bit_nv21  YUV420 8-bit NV21\n";
    std::cout << "  便捷别名:\n";
    std::cout << "    pp0_nv12, pp0_nv21, pp0_i420, pp0_yv12, pp0_p010\n";
    std::cout << "    pp0_nv16, pp0_nv61, pp0_i422, pp0_nv24, pp0_i444\n";

    std::cout << "\nPP1 RGB Format Tests (18 种 + 7 别名):\n";
    std::cout << "  RGB 10-bit 系列:\n";
    std::cout << "    pp1_argb2101010       ARGB2101010 (10-bit per channel)\n";
    std::cout << "    pp1_abgr2101010       ABGR2101010 (10-bit per channel)\n";
    std::cout << "    pp1_bgra2101010       BGRA2101010 (10-bit per channel)\n";
    std::cout << "    pp1_rgba2101010       RGBA2101010 (10-bit per channel)\n";
    std::cout << "  RGB 8-bit packed:\n";
    std::cout << "    pp1_abgr8888          ABGR8888 packed\n";
    std::cout << "    pp1_argb8888          ARGB8888 packed\n";
    std::cout << "    pp1_bgr888            BGR888 packed\n";
    std::cout << "    pp1_bgra8888          BGRA8888 packed\n";
    std::cout << "    pp1_bgrx8888          BGRX8888 packed\n";
    std::cout << "    pp1_rgb888            RGB888 packed\n";
    std::cout << "    pp1_rgba8888          RGBA8888 packed\n";
    std::cout << "    pp1_rgbx8888          RGBX8888 packed\n";
    std::cout << "    pp1_xrgb8888          XRGB8888 packed\n";
    std::cout << "    pp1_xbgr8888          XBGR8888 packed\n";
    std::cout << "  RGB 8-bit planar:\n";
    std::cout << "    pp1_rgb888_planar     RGB888 planar\n";
    std::cout << "    pp1_bgr888_planar     BGR888 planar\n";
    std::cout << "  RGB 16-bit:\n";
    std::cout << "    pp1_rgb161616         RGB 16-bit per channel\n";
    std::cout << "    pp1_bgr161616         BGR 16-bit per channel\n";
    std::cout << "    pp1_rgb161616_planar  RGB 16-bit planar\n";
    std::cout << "  便捷别名:\n";
    std::cout << "    pp1_argb888, pp1_abgr888, pp1_rgba888, pp1_bgra888\n";
    std::cout << "    pp1_r16g16b16, pp1_b16g16r16, pp1_gbrp\n";

    std::cout << "\nPP1 YUV Format Tests (16 种 + 10 别名):\n";
    std::cout << "  YUV400 系列 (灰度):\n";
    std::cout << "    pp1_yuv400_p010       YUV400 P010 (10-bit grayscale)\n";
    std::cout << "    pp1_yuv400_i010       YUV400 I010 (10-bit grayscale)\n";
    std::cout << "    pp1_yuv400_l010       YUV400 L010 (10-bit grayscale)\n";
    std::cout << "    pp1_yuv400_pack10     YUV400 Pack10 (10-bit grayscale)\n";
    std::cout << "    pp1_yuv400_8bit       YUV400 8-bit (8-bit grayscale)\n";
    std::cout << "  YUV420 NV12 系列:\n";
    std::cout << "    pp1_yuv420_nv12_p010  YUV420 NV12 P010 (10-bit)\n";
    std::cout << "    pp1_yuv420_nv12_i010  YUV420 NV12 I010 (10-bit)\n";
    std::cout << "    pp1_yuv420_nv12_l010  YUV420 NV12 L010 (10-bit)\n";
    std::cout << "    pp1_yuv420_nv12_pack10 YUV420 NV12 Pack10 (10-bit)\n";
    std::cout << "    pp1_yuv420_8bit_nv12  YUV420 8-bit NV12\n";
    std::cout << "  YUV420 NV21 系列:\n";
    std::cout << "    pp1_yuv420_nv21_p010_tiled YUV420 NV21 P010 Tiled-4x4\n";
    std::cout << "    pp1_yuv420_nv21_i011  YUV420 NV21 I011\n";
    std::cout << "    pp1_yuv420_nv21_l010  YUV420 NV21 L010\n";
    std::cout << "    pp1_yuv420_p010       YUV420 P010 (10-bit)\n";
    std::cout << "    pp1_yuv420_8bit_nv21  YUV420 8-bit NV21\n";
    std::cout << "    pp1_yuv420_nv21_p010  YUV420 NV21 P010 (PP1专属)\n";
    std::cout << "  便捷别名:\n";
    std::cout << "    pp1_nv12, pp1_nv21, pp1_i420, pp1_yv12, pp1_p010\n";
    std::cout << "    pp1_nv16, pp1_nv61, pp1_i422, pp1_nv24, pp1_i444\n";

    std::cout << "\nMulti-PP Tests (10):\n";
    std::cout << "  multi_pp_t01        PP0=NV12, PP1=RGB888\n";
    std::cout << "  multi_pp_t02        PP0=NV12, PP1=ARGB888\n";
    std::cout << "  multi_pp_t03        PP0=NV21, PP1=BGR888\n";
    std::cout << "  multi_pp_t04        PP0=NV12, PP1=RGB888 (8-bit)\n";
    std::cout << "  multi_pp_t05        PP0=P010 (10-bit), PP1=ARGB888\n";
    std::cout << "  multi_pp_t06        PP0=P010 (10-bit), PP1=R16G16B16 (16-bit)\n";
    std::cout << "  multi_pp_t07        PP0=NV12, PP1=RGB888\n";
    std::cout << "  multi_pp_t09        PP0=NV12, PP1=RGB888 planar\n";
    std::cout << "  multi_pp_t10        PP0=P010, PP1=ARGB888\n";
    std::cout << "  multi_pp_t11        PP0=NV12, PP1=NV21\n";

    std::cout << "\nCrop Tests (4):\n";
    std::cout << "  multi_pp_crop1      PP0 crop 4096x2160 -> 1920x1080\n";
    std::cout << "  multi_pp_crop2      PP0 crop 32768x32768 -> 1280x720\n";
    std::cout << "  multi_pp_crop3      PP1 crop 4096x2160 -> 1920x1080\n";
    std::cout << "  multi_pp_crop4      PP1 crop 32768x32768 -> 1280x720\n";

    std::cout << "\nScale Tests (4):\n";
    std::cout << "  multi_pp_scale1     PP0 scale 32768x32768 -> 256x256\n";
    std::cout << "  multi_pp_scale2     PP1 scale 4096x2160 -> 128x128\n";
    std::cout << "  multi_pp_scale3     PP0+PP1 dual scale 32768x32768 -> 256x256\n";
    std::cout << "  multi_pp_scale4     PP0+PP1 dual scale 4096x2160 -> 128x128\n";

    std::cout << "════════════════════════════════════════════════════════\n";
    std::cout << "PP 格式测试总计: 49 种（15 PP0 YUV + 18 PP1 RGB + 16 PP1 YUV）\n";
    std::cout << "Multi-PP 测试: 10 种\n";
    std::cout << "Crop/Scale 测试: 8 种\n";
    std::cout << "便捷别名: 27 个\n";
    std::cout << "────────────────────────────────────────────────────────\n";
    std::cout << "Total: 94 个预定义测试项（含别名）\n";
    std::cout << "\n";
}

} // namespace pp
} // namespace test
