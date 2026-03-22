/**
 * @file PPPlugin.cpp
 * @brief PPPlugin 实现
 * 
 * 重构为 IOptionPlugin 插件架构，使用 ExecuteMode 静态工具类
 */

#include "PPPlugin.hpp"
#include "../common/WorkerConfigFactory.hpp"
#include "consumptionline/BufferConsumerService.hpp"
#include "consumptionline/BufferConsumerStrategies.hpp"

#include "../common/third_party/CLI11.hpp"

#include <iostream>
#include <cstring>
#include <sstream>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

namespace test {
namespace pp {

// ========================================
// 辅助函数：解析逗号分隔的列表
// ========================================

/// 解析逗号分隔的字符串列表
static std::vector<std::string> parseStringList(const std::string& str) {
    std::vector<std::string> result;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, ',')) {
        // 去除首尾空格
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

/// 解析通道参数（支持 "0", "1", "0,1" 等格式）
/// v2.27: 移除旧格式 "pp0", "pp1", "multi"，统一使用数字格式
static std::vector<int> parseChannels(const std::string& str) {
    std::vector<int> result;
    for (const auto& s : parseStringList(str)) {
        try {
            result.push_back(std::stoi(s));
        } catch (...) {
            // 忽略无效的数字
        }
    }
    return result.empty() ? std::vector<int>{0} : result;  // 默认通道 0
}

/// 解析格式参数（支持 "nv12", "nv12,rgb888" 等格式）
static std::vector<OutputFormat> parseFormats(const std::string& str) {
    std::vector<OutputFormat> result;
    for (const auto& s : parseStringList(str)) {
        result.push_back(TacoConfigBuilder::mapFormatNameToEnum(s));
    }
    return result.empty() ? std::vector<OutputFormat>{OutputFormat::YUV_NV12} : result;
}

// 模块级日志实例
static log4cplus::Logger& getLogger() {
    static log4cplus::Logger logger = log4cplus::Logger::getInstance(
        LOG4CPLUS_TEXT("components.test.pp"));
    return logger;
}

// ========================================
// 预定义测试参数表
// ========================================

const std::map<std::string, PPTestParams>& PPPlugin::getPredefinedTests() {
    static std::map<std::string, PPTestParams> tests = {
        // ════════════════════════════════════════════════════════════════════
        // PP0 YUV 格式（15 种，对应 lfl 分支 test_decode.cpp 定义）
        // 注：YUV400 系列使用相同的底层格式，只是语义不同
        // ════════════════════════════════════════════════════════════════════
        // YUV400 系列 (灰度 10-bit)
        {"pp0_yuv400_p010",       {"0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp0_yuv400_i010",       {"0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp0_yuv400_l010",       {"0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp0_yuv400_pack10",     {"0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        // YUV400 8-bit (灰度 8-bit)
        {"pp0_yuv400_8bit",       {"0", OutputFormat::YUV_NV12, 1920, 1080, ColorStandard::BT601}},
        // YUV420 NV12 10-bit 系列
        {"pp0_yuv420_nv12_p010",  {"0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp0_yuv420_nv12_i010",  {"0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp0_yuv420_nv12_l010",  {"0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp0_yuv420_nv12_pack10",{"0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        // YUV420 8-bit NV12
        {"pp0_yuv420_8bit_nv12",  {"0", OutputFormat::YUV_NV12, 1920, 1080, ColorStandard::BT601}},
        // YUV420 NV21 10-bit 系列
        {"pp0_yuv420_nv21_p010_tiled", {"0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp0_yuv420_nv21_i011",  {"0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp0_yuv420_nv21_l010",  {"0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        // YUV420 P010
        {"pp0_yuv420_p010",       {"0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        // YUV420 8-bit NV21
        {"pp0_yuv420_8bit_nv21",  {"0", OutputFormat::YUV_NV21, 1920, 1080, ColorStandard::BT601}},
        
        // 便捷别名（兼容旧测试名）
        {"pp0_nv12",              {"0", OutputFormat::YUV_NV12, 1920, 1080}},
        {"pp0_nv21",              {"0", OutputFormat::YUV_NV21, 1920, 1080}},
        {"pp0_i420",              {"0", OutputFormat::YUV_I420, 1920, 1080}},
        {"pp0_yv12",              {"0", OutputFormat::YUV_YV12, 1920, 1080}},
        {"pp0_p010",              {"0", OutputFormat::YUV_P010, 1920, 1080}},
        {"pp0_nv16",              {"0", OutputFormat::YUV_NV16, 1920, 1080}},
        {"pp0_nv61",              {"0", OutputFormat::YUV_NV61, 1920, 1080}},
        {"pp0_i422",              {"0", OutputFormat::YUV_I422, 1920, 1080}},
        {"pp0_nv24",              {"0", OutputFormat::YUV_NV24, 1920, 1080}},
        {"pp0_i444",              {"0", OutputFormat::YUV_I444, 1920, 1080}},
        
        // ════════════════════════════════════════════════════════════════════
        // PP1 RGB 格式（18 种，对应 lfl 分支 test_decode.cpp 定义）
        // ════════════════════════════════════════════════════════════════════
        // RGB 10-bit 系列（使用 16-bit 格式实现）
        {"pp1_argb2101010",       {"1", OutputFormat::RGB_ARGB888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_abgr2101010",       {"1", OutputFormat::RGB_ABGR888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_bgra2101010",       {"1", OutputFormat::RGB_BGRA888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_rgba2101010",       {"1", OutputFormat::RGB_RGBA888, 1920, 1080, ColorStandard::BT601}},
        // RGB 8-bit 系列 - packed
        {"pp1_abgr8888",          {"1", OutputFormat::RGB_ABGR888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_argb8888",          {"1", OutputFormat::RGB_ARGB888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_bgr888",            {"1", OutputFormat::RGB_BGR888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_bgra8888",          {"1", OutputFormat::RGB_BGRA888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_bgrx8888",          {"1", OutputFormat::RGB_BGRX888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_rgb888",            {"1", OutputFormat::RGB_RGB888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_rgba8888",          {"1", OutputFormat::RGB_RGBA888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_rgbx8888",          {"1", OutputFormat::RGB_RGBX888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_xrgb8888",          {"1", OutputFormat::RGB_XRGB888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_xbgr8888",          {"1", OutputFormat::RGB_XBGR888, 1920, 1080, ColorStandard::BT601}},
        // RGB 8-bit 系列 - planar
        {"pp1_rgb888_planar",     {"1", OutputFormat::RGB_RGB888_PLANAR, 1920, 1080, ColorStandard::BT601}},
        {"pp1_bgr888_planar",     {"1", OutputFormat::RGB_BGR888_PLANAR, 1920, 1080, ColorStandard::BT601}},
        // RGB 16-bit 系列
        {"pp1_rgb161616",         {"1", OutputFormat::RGB_R16G16B16, 1920, 1080, ColorStandard::BT601}},
        {"pp1_bgr161616",         {"1", OutputFormat::RGB_B16G16R16, 1920, 1080, ColorStandard::BT601}},
        {"pp1_rgb161616_planar",  {"1", OutputFormat::RGB_GBRP, 1920, 1080, ColorStandard::BT601}},
        
        // 便捷别名（兼容旧测试名）
        {"pp1_argb888",           {"1", OutputFormat::RGB_ARGB888, 1920, 1080}},
        {"pp1_abgr888",           {"1", OutputFormat::RGB_ABGR888, 1920, 1080}},
        {"pp1_rgba888",           {"1", OutputFormat::RGB_RGBA888, 1920, 1080}},
        {"pp1_bgra888",           {"1", OutputFormat::RGB_BGRA888, 1920, 1080}},
        {"pp1_r16g16b16",         {"1", OutputFormat::RGB_R16G16B16, 1920, 1080}},
        {"pp1_b16g16r16",         {"1", OutputFormat::RGB_B16G16R16, 1920, 1080}},
        {"pp1_gbrp",              {"1", OutputFormat::RGB_GBRP, 1920, 1080}},
        
        // ════════════════════════════════════════════════════════════════════
        // PP1 YUV 格式（16 种，对应 lfl 分支 test_decode.cpp 定义）
        // ════════════════════════════════════════════════════════════════════
        // YUV400 系列 (灰度 10-bit)
        {"pp1_yuv400_p010",       {"1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp1_yuv400_i010",       {"1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp1_yuv400_l010",       {"1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp1_yuv400_pack10",     {"1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        // YUV400 8-bit (灰度 8-bit)
        {"pp1_yuv400_8bit",       {"1", OutputFormat::YUV_NV12, 1920, 1080, ColorStandard::BT601}},
        // YUV420 NV12 10-bit 系列
        {"pp1_yuv420_nv12_p010",  {"1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp1_yuv420_nv12_i010",  {"1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp1_yuv420_nv12_l010",  {"1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp1_yuv420_nv12_pack10",{"1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        // YUV420 8-bit NV12
        {"pp1_yuv420_8bit_nv12",  {"1", OutputFormat::YUV_NV12, 1920, 1080, ColorStandard::BT601}},
        // YUV420 NV21 10-bit 系列
        {"pp1_yuv420_nv21_p010_tiled", {"1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp1_yuv420_nv21_i011",  {"1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp1_yuv420_nv21_l010",  {"1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        // YUV420 P010
        {"pp1_yuv420_p010",       {"1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        // YUV420 8-bit NV21
        {"pp1_yuv420_8bit_nv21",  {"1", OutputFormat::YUV_NV21, 1920, 1080, ColorStandard::BT601}},
        // YUV420 NV21 P010（第16种，PP1比PP0多一种）
        {"pp1_yuv420_nv21_p010",  {"1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        
        // 便捷别名（兼容旧测试名）
        {"pp1_nv12",              {"1", OutputFormat::YUV_NV12, 1920, 1080}},
        {"pp1_nv21",              {"1", OutputFormat::YUV_NV21, 1920, 1080}},
        {"pp1_i420",              {"1", OutputFormat::YUV_I420, 1920, 1080}},
        {"pp1_yv12",              {"1", OutputFormat::YUV_YV12, 1920, 1080}},
        {"pp1_p010",              {"1", OutputFormat::YUV_P010, 1920, 1080}},
        {"pp1_nv16",              {"1", OutputFormat::YUV_NV16, 1920, 1080}},
        {"pp1_nv61",              {"1", OutputFormat::YUV_NV61, 1920, 1080}},
        {"pp1_i422",              {"1", OutputFormat::YUV_I422, 1920, 1080}},
        {"pp1_nv24",              {"1", OutputFormat::YUV_NV24, 1920, 1080}},
        {"pp1_i444",              {"1", OutputFormat::YUV_I444, 1920, 1080}},
        
        // ========================================
        // Multi-PP 测试（10 个，对应原始 test_pp.cpp）
        // ========================================
        // T01: PP0=YUV420 8-bit NV12, PP1=RGB888
        {"multi_pp_t01",    {OutputFormat::YUV_NV12, OutputFormat::RGB_RGB888, 1920, 1080}},
        // T02: PP0=YUV420 8-bit NV12, PP1=ARGB8888
        {"multi_pp_t02",    {OutputFormat::YUV_NV12, OutputFormat::RGB_ARGB888, 1920, 1080}},
        // T03: PP0=YUV420 8-bit NV21, PP1=BGR888
        {"multi_pp_t03",    {OutputFormat::YUV_NV21, OutputFormat::RGB_BGR888, 1920, 1080}},
        // T04: PP0=YUV420 8-bit NV12, PP1=RGB888 (8-bit)
        {"multi_pp_t04",    {OutputFormat::YUV_NV12, OutputFormat::RGB_RGB888, 1920, 1080}},
        // T05: PP0=YUV420 P010 (10-bit), PP1=ARGB2101010 (10-bit)
        {"multi_pp_t05",    {OutputFormat::YUV_P010, OutputFormat::RGB_R16G16B16, 1920, 1080}},
        // T06: PP0=YUV420 I010 (10-bit), PP1=RGB161616 (16-bit)
        {"multi_pp_t06",    {OutputFormat::YUV_P010, OutputFormat::RGB_R16G16B16, 1920, 1080}},
        // T07: PP0=YUV400 8-bit, PP1=RGB888
        {"multi_pp_t07",    {OutputFormat::YUV_NV12, OutputFormat::RGB_RGB888, 1920, 1080}},
        // T09: PP0=YUV420 8-bit NV12, PP1=RGB888 planar
        {"multi_pp_t09",    {OutputFormat::YUV_NV12, OutputFormat::RGB_RGB888_PLANAR, 1920, 1080}},
        // T10: PP0=YUV420 NV21 P010 Tiled-4x4, PP1=ARGB8888
        {"multi_pp_t10",    {OutputFormat::YUV_P010, OutputFormat::RGB_ARGB888, 1920, 1080}},
        // T11: PP0=YUV420 8-bit NV12, PP1=YUV420 8-bit NV21
        {"multi_pp_t11",    {OutputFormat::YUV_NV12, OutputFormat::YUV_NV21, 1920, 1080}},
        
        // ========================================
        // Multi-PP Crop/Scale 测试（8 个，对应 lfl 分支 test_decode.cpp）
        // ========================================
        // Crop 测试（4 个）
        // Crop1: PP0 crop 4096x2160 -> 1920x1080
        {"multi_pp_crop1",  {"0", OutputFormat::YUV_NV12, 1920, 1080, ColorStandard::BT709, 0, 0, 4096, 2160}},
        // Crop2: PP0 crop 32768x32768 -> 1280x720
        {"multi_pp_crop2",  {"0", OutputFormat::YUV_NV12, 1280, 720, ColorStandard::BT709, 0, 0, 32768, 32768}},
        // Crop3: PP1 crop 4096x2160 -> 1920x1080
        {"multi_pp_crop3",  {"1", OutputFormat::YUV_NV12, 1920, 1080, ColorStandard::BT709, 0, 0, 4096, 2160}},
        // Crop4: PP1 crop 32768x32768 -> 1280x720
        {"multi_pp_crop4",  {"1", OutputFormat::YUV_NV12, 1280, 720, ColorStandard::BT709, 0, 0, 32768, 32768}},
        
        // Scale 测试（4 个）
        // Scale1: PP0 down-scale 32768x32768 -> 256x256
        {"multi_pp_scale1", {"0", OutputFormat::YUV_NV12, 256, 256, ColorStandard::BT709}},
        // Scale2: PP1 down-scale 4096x2160 -> 128x128
        {"multi_pp_scale2", {"1", OutputFormat::YUV_NV12, 128, 128, ColorStandard::BT709}},
        // Scale3: PP0+PP1 双通道 down-scale 32768x32768 -> 256x256
        {"multi_pp_scale3", {OutputFormat::YUV_NV12, OutputFormat::YUV_NV12, 256, 256, ColorStandard::BT709}},
        // Scale4: PP0+PP1 双通道 down-scale 4096x2160 -> 128x128
        {"multi_pp_scale4", {OutputFormat::YUV_NV12, OutputFormat::YUV_NV12, 128, 128, ColorStandard::BT709}},
        
        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - PP Crop 测试（带分辨率）
        // ════════════════════════════════════════════════════════════════════
        // PP0 Crop
        {"pp0_720p_crop",           {"0", OutputFormat::YUV_NV12, 1280, 720, ColorStandard::BT709, 0, 0, 1280, 720}},
        {"pp0_1080p_crop",          {"0", OutputFormat::YUV_NV12, 1920, 1080, ColorStandard::BT709, 0, 0, 1920, 1080}},
        // PP1 RGB Crop
        {"pp1_720p_rgb_crop",       {"1", OutputFormat::RGB_RGB888, 1280, 720, ColorStandard::BT709, 0, 0, 1280, 720}},
        {"pp1_1080p_rgb_crop",      {"1", OutputFormat::RGB_RGB888, 1920, 1080, ColorStandard::BT709, 0, 0, 1920, 1080}},
        // PP1 YUV Crop
        {"pp1_720p_yuv_crop",       {"1", OutputFormat::YUV_NV12, 1280, 720, ColorStandard::BT709, 0, 0, 1280, 720}},
        {"pp1_1080p_yuv_crop",      {"1", OutputFormat::YUV_NV12, 1920, 1080, ColorStandard::BT709, 0, 0, 1920, 1080}},
        
        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - Multi-PP 扩展测试
        // ════════════════════════════════════════════════════════════════════
        {"multi_pp",                {OutputFormat::YUV_NV12, OutputFormat::RGB_RGB888, 1920, 1080}},
        {"multi_pp_crop",           {OutputFormat::YUV_NV12, OutputFormat::RGB_RGB888, 1920, 1080, ColorStandard::BT709}},
        {"multi_pp_scale",          {OutputFormat::YUV_NV12, OutputFormat::RGB_RGB888, 960, 540, ColorStandard::BT709}},
        {"multi_pp_crop_scale",     {OutputFormat::YUV_NV12, OutputFormat::RGB_RGB888, 960, 540, ColorStandard::BT709}},
        
        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - PP0 带分辨率的格式测试
        // ════════════════════════════════════════════════════════════════════
        {"pp0_720p_nv12",           {"0", OutputFormat::YUV_NV12, 1280, 720}},
        {"pp0_720p_p010",           {"0", OutputFormat::YUV_P010, 1280, 720}},
        {"pp0_1080p_nv21",          {"0", OutputFormat::YUV_NV21, 1920, 1080}},
        {"pp0_4k_nv12",             {"0", OutputFormat::YUV_NV12, 3840, 2160}},
        
        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - PP1 带分辨率的格式测试
        // ════════════════════════════════════════════════════════════════════
        {"pp1_720p_argb8888",       {"1", OutputFormat::RGB_ARGB888, 1280, 720}},
        {"pp1_4k_argb8888",         {"1", OutputFormat::RGB_ARGB888, 3840, 2160}},
        {"pp1_720p_rgb888",         {"1", OutputFormat::RGB_RGB888, 1280, 720}},
        {"pp1_1080p_argb8888",      {"1", OutputFormat::RGB_ARGB888, 1920, 1080}},
        
        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - Crop 带输出分辨率
        // ════════════════════════════════════════════════════════════════════
        {"crop_720p_1024x576",      {"0", OutputFormat::YUV_NV12, 1024, 576, ColorStandard::BT709, 0, 0, 1280, 720}},
        {"crop_1080p_1600x900",     {"0", OutputFormat::YUV_NV12, 1600, 900, ColorStandard::BT709, 0, 0, 1920, 1080}},
        
        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - Scale 带输出分辨率
        // ════════════════════════════════════════════════════════════════════
        {"scale_720p_512x288",      {"0", OutputFormat::YUV_NV12, 512, 288, ColorStandard::BT709}},
        {"scale_1080p_800x450",     {"0", OutputFormat::YUV_NV12, 800, 450, ColorStandard::BT709}},
        
        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - H265 PP0/PP1 格式测试
        // ════════════════════════════════════════════════════════════════════
        {"pp0_h265_720p_nv12",      {"0", OutputFormat::YUV_NV12, 1280, 720}},
        {"pp0_h265_1080p_p010",     {"0", OutputFormat::YUV_P010, 1920, 1080}},
        {"pp0_h265_4k_nv12",        {"0", OutputFormat::YUV_NV12, 3840, 2160}},
        {"pp1_h265_720p_rgb888",    {"1", OutputFormat::RGB_RGB888, 1280, 720}},
        {"pp1_h265_1080p_argb8888", {"1", OutputFormat::RGB_ARGB888, 1920, 1080}},
        
        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - H265 Crop+Scale
        // ════════════════════════════════════════════════════════════════════
        {"h265_1080p_crop_scale",   {"0", OutputFormat::YUV_NV12, 960, 540, ColorStandard::BT709, 0, 0, 1920, 1080}},
    };
    return tests;
}

// ========================================
// IOptionPlugin 接口实现
// ========================================

void PPPlugin::registerOptions(CLI::App& app) {
    app.add_flag("-l,--list", show_list_, "列出所有预定义测试");
    app.add_option("-i,--input", input_path_, "输入视频路径");
    app.add_option("-D,--decoder", decoder_str_, "解码方式 (hw|sw, 默认: hw)");
    app.add_option("-f,--format", format_str_, "输出格式, 多通道用逗号分隔");
    app.add_option("-c,--channel", channel_str_, "通道选择 (0|1|0,1)");
    app.add_option("-W,--width", params_.width, "输出宽度");
    app.add_option("-H,--height", params_.height, "输出高度");
    app.add_option("-R,--resolution", resolution_str_, "分辨率 (如 1920x1080)");
    app.add_option("-C,--crop", crop_str_, "裁剪区域 (x,y,w,h)");
    app.add_option("-s,--color-std", color_std_str_, "颜色标准 (bt601|bt709|bt2020)");
    app.add_option("-o,--output", save_paths_, "输出文件路径")->delimiter(',');
    app.add_option("-n,--save", save_frames_, "保存帧数")->delimiter(',');
    app.add_option("-m,--max-frames", max_frames_, "最大帧数（数据源读取与消费循环共用，0=不覆盖）");
    app.add_flag("-p,--psnr", enable_psnr_, "启用 PSNR 验证");
    app.add_flag("-S,--ssim", enable_ssim_, "启用 SSIM 验证");
    app.add_option("-P,--min-psnr", min_psnr_, "PSNR 阈值");
    app.add_option("-M,--min-ssim", min_ssim_, "SSIM 阈值");
    app.add_flag("-v,--verbose", verbose_, "详细日志");
    app.add_option("positional", positional_args_, "测试名或输入文件路径");

    app.footer(
        "Examples:\n"
        "  qa_cases pp video.mp4\n"
        "  qa_cases pp --channel 0 --format nv12 video.mp4\n"
        "  qa_cases pp --channel 1 --format argb888 video.mp4\n"
        "  qa_cases pp --psnr video.mp4\n"
        "  qa_cases pp pp1_argb888 video.mp4\n"
    );
}

void PPPlugin::applyTo(WorkerConfig& config) const {
    if (enable_psnr_)
        config.consumer_type.compare.enable_psnr = true;
    if (enable_ssim_)
        config.consumer_type.compare.enable_ssim = true;
    if (min_psnr_ >= 0)
        config.consumer_type.compare.min_psnr = min_psnr_;
    if (min_ssim_ >= 0)
        config.consumer_type.compare.min_ssim = min_ssim_;
    if (verbose_)
        config.consumer_type.verbose = true;
    if (!save_paths_.empty()) {
        config.consumer_type.save_raw.enable = true;
        config.consumer_type.save_raw.output_paths = save_paths_;
    }
    if (!save_frames_.empty())
        config.consumer_type.save_raw.max_frames_per_channel = save_frames_;
    if (max_frames_ != 0)
        config.consumer_type.max_frames = max_frames_;
    config.data_source = DataSourceConfigBuilder(config.data_source)
        .setPathIfNonEmpty(input_path_)
        .setMaxFramesIfNonZero(max_frames_)
        .build();
}

int PPPlugin::handlePreActions() {
    if (!decoder_str_.empty()) {
        if (decoder_str_ == "sw" || decoder_str_ == "software")
            params_.use_hardware = false;
    }
    if (!resolution_str_.empty()) {
        size_t pos = resolution_str_.find('x');
        if (pos != std::string::npos) {
            params_.width = std::stoi(resolution_str_.substr(0, pos));
            params_.height = std::stoi(resolution_str_.substr(pos + 1));
        }
    }
    if (!crop_str_.empty()) {
        int vals[4] = {0}; int idx = 0;
        size_t start = 0, end;
        while ((end = crop_str_.find(',', start)) != std::string::npos && idx < 4) {
            vals[idx++] = std::stoi(crop_str_.substr(start, end - start));
            start = end + 1;
        }
        if (idx < 4 && start < crop_str_.length())
            vals[idx] = std::stoi(crop_str_.substr(start));
        params_.crop_x = vals[0]; params_.crop_y = vals[1];
        params_.crop_w = vals[2]; params_.crop_h = vals[3];
    }
    if (!channel_str_.empty()) params_.channels = parseChannels(channel_str_);
    if (!format_str_.empty() && format_str_ != "nv12") params_.formats = parseFormats(format_str_);

    for (const auto& a : positional_args_) {
        const auto& tests = getPredefinedTests();
        auto it = tests.find(a);
        if (it != tests.end()) { params_ = it->second; continue; }
        if (input_path_.empty()) input_path_ = a;
    }

    if (show_list_) { listTests(); return 0; }
    if (input_path_.empty()) {
        LOG4CPLUS_ERROR(getLogger(), "No input file specified");
        return 1;
    }
    return -1;
}

std::string PPPlugin::getTestName() const {
    PPTestParams params = params_;
    if (params.channels.empty()) params.channels = {0};
    if (params.formats.empty()) params.formats = parseFormats(format_str_);

    std::ostringstream test_name;
    test_name << params.getChannelString();
    for (size_t i = 0; i < params.formats.size(); ++i) {
        test_name << (i == 0 ? " " : ",")
                  << TacoConfigBuilder::mapFormatEnumToName(params.formats[i]);
    }
    test_name << " (" << params.width << "x" << params.height << ")";
    return test_name.str();
}

std::vector<WorkerConfig> PPPlugin::buildPipelineConfigs(const WorkerConfig& shared_config) {
    if (input_path_.empty()) return {};

    // applyTo 写入的 data_source 读帧上限 / loop 需合并进工厂生成的 config（工厂默认可能为 -1/false）
    auto sync_data_source_caps = [](WorkerConfig& cfg, const WorkerConfig& shared) {
        cfg.data_source = DataSourceConfigBuilder(cfg.data_source)
            .setMaxFrames(shared.data_source.max_frames)
            .setLoop(shared.data_source.loop)
            .build();
    };

    PPTestParams params = params_;

    if (params.channels.empty()) {
        if (channel_str_.empty()) params.channels = {0};
        if (params.formats.empty()) params.formats = parseFormats(format_str_);
        params.color_std = TacoConfigBuilder::mapColorStdNameToEnum(color_std_str_);

        std::string channels_info, formats_info;
        for (size_t i = 0; i < params.channels.size(); ++i) {
            if (i > 0) channels_info += ",";
            channels_info += std::to_string(params.channels[i]);
        }
        for (size_t i = 0; i < params.formats.size(); ++i) {
            if (i > 0) formats_info += ",";
            formats_info += std::string(TacoConfigBuilder::mapFormatEnumToName(params.formats[i]));
        }
        LOG4CPLUS_INFO_FMT(getLogger(),
            "Using PP configuration: channels=[%s], formats=[%s] (use -c and -f to customize)",
            channels_info.c_str(), formats_info.c_str());
    }

    // COMPARE：多通道 → 通道比较（channel compare）
    if ((shared_config.consumer_type.compare.enable_psnr || shared_config.consumer_type.compare.enable_ssim)
        && params.channels.size() >= 2) {
        WorkerConfig full_config = buildConfig(input_path_, params);
        full_config.consumer_type = shared_config.consumer_type;
        full_config.consumer_type.compare.enable_channel_compare = true;
        full_config.consumer_type.compare.reference_channel = params.channels[0];
        full_config.consumer_type.compare.compare_channel = params.channels[1];
        sync_data_source_caps(full_config, shared_config);
        return {full_config};
    }

    // COMPARE：单通道 → HW vs SW
    if (shared_config.consumer_type.compare.enable_psnr || shared_config.consumer_type.compare.enable_ssim) {
        auto hw_config = buildConfig(input_path_, params);
        hw_config.consumer_type = shared_config.consumer_type;
        sync_data_source_caps(hw_config, shared_config);

        auto sw_config = common::WorkerConfigFactory::createSoftwareDecode(
            input_path_, params.width, params.height);
        sw_config.consumer_type = shared_config.consumer_type;
        sync_data_source_caps(sw_config, shared_config);

        return {hw_config, sw_config};
    }

    // SINGLE
    WorkerConfig full_config = buildConfig(input_path_, params);
    full_config.consumer_type = shared_config.consumer_type;
    sync_data_source_caps(full_config, shared_config);
    return {full_config};
}

// ========================================
// 核心辅助方法实现
// ========================================

WorkerConfig PPPlugin::buildConfig(const std::string& path, const PPTestParams& params) {
    WorkerConfig config;
    
    // 软件解码模式：不使用硬件 PP，直接解码
    if (!params.use_hardware) {
        config = common::WorkerConfigFactory::createSoftwareDecode(
            path, params.width, params.height);
        LOG4CPLUS_WARN(getLogger(), 
            "Software decode mode: Hardware PP features are not available");
        return config;
    }
    
    // 硬件解码模式：使用 TACO PP
    // 根据 channels 列表决定使用哪种配置
    if (params.channels.empty() || (params.channels.size() == 1 && params.channels[0] == 0)) {
        // 单通道 PP0
        config = common::WorkerConfigFactory::createPP0YuvConfig(
            path, params.getFormat(0), params.width, params.height, params.color_std);
    } else if (params.channels.size() == 1 && params.channels[0] == 1) {
        // 单通道 PP1
        OutputFormat fmt = params.getFormat(0);
        int fmt_val = static_cast<int>(fmt);
        if (fmt_val >= 1000) {
            config = common::WorkerConfigFactory::createPP1RgbConfig(
                path, fmt, params.width, params.height, params.color_std);
        } else {
            config = common::WorkerConfigFactory::createPP1YuvConfig(
                path, fmt, params.width, params.height, params.color_std);
        }
    } else if (params.isMultiChannel()) {
        // 多通道模式
        OutputFormat pp0_fmt = params.getFormat(0);
        OutputFormat pp1_fmt = params.getFormat(1);
        config = common::WorkerConfigFactory::createMultiPPConfig(
            path, pp0_fmt, pp1_fmt, 
            params.width, params.height, params.color_std);
    }
    
    // 应用裁剪参数（如果有）
    if (params.crop_w > 0 && params.crop_h > 0) {
        config = common::WorkerConfigFactory::createCropConfig(
            path, params.crop_x, params.crop_y, params.crop_w, params.crop_h,
            params.width, params.height);
    }
    
    return config;
}

void PPPlugin::listTests() const {
    std::cout << "\nAvailable PP tests:\n";
    std::cout << "════════════════════════════════════════════════════════\n";
    
    // PP0 YUV 格式（15 种，对应 lfl 分支定义）
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
    
    // PP1 RGB 格式（18 种）
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
    
    // PP1 YUV 格式（16 种）
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
