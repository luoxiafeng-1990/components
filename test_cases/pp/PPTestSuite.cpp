/**
 * @file PPTestSuite.cpp
 * @brief PPTestSuite 实现
 */

#include "PPTestSuite.hpp"
#include "../common/WorkerConfigFactory.hpp"
#include "productionline/io/BufferConsumerService.hpp"

#include <iostream>
#include <getopt.h>
#include <cstring>
#include <sstream>
#include <log4cplus/loggingmacros.h>

namespace test {
namespace pp {

// 获取模块级日志实例
log4cplus::Logger& PPTestSuite::getLogger() {
    static log4cplus::Logger logger = log4cplus::Logger::getInstance(
        LOG4CPLUS_TEXT("components.test.PPSuite"));
    return logger;
}

// ========================================
// 预定义测试参数表
// ========================================

const std::map<std::string, PPTestParams>& PPTestSuite::getPredefinedTests() {
    static std::map<std::string, PPTestParams> tests = {
        // ════════════════════════════════════════════════════════════════════
        // PP0 YUV 格式（15 种，对应 lfl 分支 test_decode.cpp 定义）
        // 注：YUV400 系列使用相同的底层格式，只是语义不同
        // ════════════════════════════════════════════════════════════════════
        // YUV400 系列 (灰度 10-bit)
        {"pp0_yuv400_p010",       {"pp0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp0_yuv400_i010",       {"pp0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp0_yuv400_l010",       {"pp0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp0_yuv400_pack10",     {"pp0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        // YUV400 8-bit (灰度 8-bit)
        {"pp0_yuv400_8bit",       {"pp0", OutputFormat::YUV_NV12, 1920, 1080, ColorStandard::BT601}},
        // YUV420 NV12 10-bit 系列
        {"pp0_yuv420_nv12_p010",  {"pp0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp0_yuv420_nv12_i010",  {"pp0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp0_yuv420_nv12_l010",  {"pp0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp0_yuv420_nv12_pack10",{"pp0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        // YUV420 8-bit NV12
        {"pp0_yuv420_8bit_nv12",  {"pp0", OutputFormat::YUV_NV12, 1920, 1080, ColorStandard::BT601}},
        // YUV420 NV21 10-bit 系列
        {"pp0_yuv420_nv21_p010_tiled", {"pp0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp0_yuv420_nv21_i011",  {"pp0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp0_yuv420_nv21_l010",  {"pp0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        // YUV420 P010
        {"pp0_yuv420_p010",       {"pp0", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        // YUV420 8-bit NV21
        {"pp0_yuv420_8bit_nv21",  {"pp0", OutputFormat::YUV_NV21, 1920, 1080, ColorStandard::BT601}},
        
        // 便捷别名（兼容旧测试名）
        {"pp0_nv12",              {"pp0", OutputFormat::YUV_NV12, 1920, 1080}},
        {"pp0_nv21",              {"pp0", OutputFormat::YUV_NV21, 1920, 1080}},
        {"pp0_i420",              {"pp0", OutputFormat::YUV_I420, 1920, 1080}},
        {"pp0_yv12",              {"pp0", OutputFormat::YUV_YV12, 1920, 1080}},
        {"pp0_p010",              {"pp0", OutputFormat::YUV_P010, 1920, 1080}},
        {"pp0_nv16",              {"pp0", OutputFormat::YUV_NV16, 1920, 1080}},
        {"pp0_nv61",              {"pp0", OutputFormat::YUV_NV61, 1920, 1080}},
        {"pp0_i422",              {"pp0", OutputFormat::YUV_I422, 1920, 1080}},
        {"pp0_nv24",              {"pp0", OutputFormat::YUV_NV24, 1920, 1080}},
        {"pp0_i444",              {"pp0", OutputFormat::YUV_I444, 1920, 1080}},
        
        // ════════════════════════════════════════════════════════════════════
        // PP1 RGB 格式（18 种，对应 lfl 分支 test_decode.cpp 定义）
        // ════════════════════════════════════════════════════════════════════
        // RGB 10-bit 系列（使用 16-bit 格式实现）
        {"pp1_argb2101010",       {"pp1", OutputFormat::RGB_ARGB888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_abgr2101010",       {"pp1", OutputFormat::RGB_ABGR888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_bgra2101010",       {"pp1", OutputFormat::RGB_BGRA888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_rgba2101010",       {"pp1", OutputFormat::RGB_RGBA888, 1920, 1080, ColorStandard::BT601}},
        // RGB 8-bit 系列 - packed
        {"pp1_abgr8888",          {"pp1", OutputFormat::RGB_ABGR888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_argb8888",          {"pp1", OutputFormat::RGB_ARGB888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_bgr888",            {"pp1", OutputFormat::RGB_BGR888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_bgra8888",          {"pp1", OutputFormat::RGB_BGRA888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_bgrx8888",          {"pp1", OutputFormat::RGB_BGRX888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_rgb888",            {"pp1", OutputFormat::RGB_RGB888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_rgba8888",          {"pp1", OutputFormat::RGB_RGBA888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_rgbx8888",          {"pp1", OutputFormat::RGB_RGBX888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_xrgb8888",          {"pp1", OutputFormat::RGB_XRGB888, 1920, 1080, ColorStandard::BT601}},
        {"pp1_xbgr8888",          {"pp1", OutputFormat::RGB_XBGR888, 1920, 1080, ColorStandard::BT601}},
        // RGB 8-bit 系列 - planar
        {"pp1_rgb888_planar",     {"pp1", OutputFormat::RGB_RGB888_PLANAR, 1920, 1080, ColorStandard::BT601}},
        {"pp1_bgr888_planar",     {"pp1", OutputFormat::RGB_BGR888_PLANAR, 1920, 1080, ColorStandard::BT601}},
        // RGB 16-bit 系列
        {"pp1_rgb161616",         {"pp1", OutputFormat::RGB_R16G16B16, 1920, 1080, ColorStandard::BT601}},
        {"pp1_bgr161616",         {"pp1", OutputFormat::RGB_B16G16R16, 1920, 1080, ColorStandard::BT601}},
        {"pp1_rgb161616_planar",  {"pp1", OutputFormat::RGB_GBRP, 1920, 1080, ColorStandard::BT601}},
        
        // 便捷别名（兼容旧测试名）
        {"pp1_argb888",           {"pp1", OutputFormat::RGB_ARGB888, 1920, 1080}},
        {"pp1_abgr888",           {"pp1", OutputFormat::RGB_ABGR888, 1920, 1080}},
        {"pp1_rgba888",           {"pp1", OutputFormat::RGB_RGBA888, 1920, 1080}},
        {"pp1_bgra888",           {"pp1", OutputFormat::RGB_BGRA888, 1920, 1080}},
        {"pp1_r16g16b16",         {"pp1", OutputFormat::RGB_R16G16B16, 1920, 1080}},
        {"pp1_b16g16r16",         {"pp1", OutputFormat::RGB_B16G16R16, 1920, 1080}},
        {"pp1_gbrp",              {"pp1", OutputFormat::RGB_GBRP, 1920, 1080}},
        
        // ════════════════════════════════════════════════════════════════════
        // PP1 YUV 格式（16 种，对应 lfl 分支 test_decode.cpp 定义）
        // ════════════════════════════════════════════════════════════════════
        // YUV400 系列 (灰度 10-bit)
        {"pp1_yuv400_p010",       {"pp1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp1_yuv400_i010",       {"pp1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp1_yuv400_l010",       {"pp1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp1_yuv400_pack10",     {"pp1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        // YUV400 8-bit (灰度 8-bit)
        {"pp1_yuv400_8bit",       {"pp1", OutputFormat::YUV_NV12, 1920, 1080, ColorStandard::BT601}},
        // YUV420 NV12 10-bit 系列
        {"pp1_yuv420_nv12_p010",  {"pp1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp1_yuv420_nv12_i010",  {"pp1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp1_yuv420_nv12_l010",  {"pp1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp1_yuv420_nv12_pack10",{"pp1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        // YUV420 8-bit NV12
        {"pp1_yuv420_8bit_nv12",  {"pp1", OutputFormat::YUV_NV12, 1920, 1080, ColorStandard::BT601}},
        // YUV420 NV21 10-bit 系列
        {"pp1_yuv420_nv21_p010_tiled", {"pp1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp1_yuv420_nv21_i011",  {"pp1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        {"pp1_yuv420_nv21_l010",  {"pp1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        // YUV420 P010
        {"pp1_yuv420_p010",       {"pp1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        // YUV420 8-bit NV21
        {"pp1_yuv420_8bit_nv21",  {"pp1", OutputFormat::YUV_NV21, 1920, 1080, ColorStandard::BT601}},
        // YUV420 NV21 P010（第16种，PP1比PP0多一种）
        {"pp1_yuv420_nv21_p010",  {"pp1", OutputFormat::YUV_P010, 1920, 1080, ColorStandard::BT2020}},
        
        // 便捷别名（兼容旧测试名）
        {"pp1_nv12",              {"pp1", OutputFormat::YUV_NV12, 1920, 1080}},
        {"pp1_nv21",              {"pp1", OutputFormat::YUV_NV21, 1920, 1080}},
        {"pp1_i420",              {"pp1", OutputFormat::YUV_I420, 1920, 1080}},
        {"pp1_yv12",              {"pp1", OutputFormat::YUV_YV12, 1920, 1080}},
        {"pp1_p010",              {"pp1", OutputFormat::YUV_P010, 1920, 1080}},
        {"pp1_nv16",              {"pp1", OutputFormat::YUV_NV16, 1920, 1080}},
        {"pp1_nv61",              {"pp1", OutputFormat::YUV_NV61, 1920, 1080}},
        {"pp1_i422",              {"pp1", OutputFormat::YUV_I422, 1920, 1080}},
        {"pp1_nv24",              {"pp1", OutputFormat::YUV_NV24, 1920, 1080}},
        {"pp1_i444",              {"pp1", OutputFormat::YUV_I444, 1920, 1080}},
        
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
        {"multi_pp_crop1",  {"pp0", OutputFormat::YUV_NV12, 1920, 1080, ColorStandard::BT709, 0, 0, 4096, 2160}},
        // Crop2: PP0 crop 32768x32768 -> 1280x720
        {"multi_pp_crop2",  {"pp0", OutputFormat::YUV_NV12, 1280, 720, ColorStandard::BT709, 0, 0, 32768, 32768}},
        // Crop3: PP1 crop 4096x2160 -> 1920x1080
        {"multi_pp_crop3",  {"pp1", OutputFormat::YUV_NV12, 1920, 1080, ColorStandard::BT709, 0, 0, 4096, 2160}},
        // Crop4: PP1 crop 32768x32768 -> 1280x720
        {"multi_pp_crop4",  {"pp1", OutputFormat::YUV_NV12, 1280, 720, ColorStandard::BT709, 0, 0, 32768, 32768}},
        
        // Scale 测试（4 个）
        // Scale1: PP0 down-scale 32768x32768 -> 256x256
        {"multi_pp_scale1", {"pp0", OutputFormat::YUV_NV12, 256, 256, ColorStandard::BT709}},
        // Scale2: PP1 down-scale 4096x2160 -> 128x128
        {"multi_pp_scale2", {"pp1", OutputFormat::YUV_NV12, 128, 128, ColorStandard::BT709}},
        // Scale3: PP0+PP1 双通道 down-scale 32768x32768 -> 256x256
        {"multi_pp_scale3", {OutputFormat::YUV_NV12, OutputFormat::YUV_NV12, 256, 256, ColorStandard::BT709}},
        // Scale4: PP0+PP1 双通道 down-scale 4096x2160 -> 128x128
        {"multi_pp_scale4", {OutputFormat::YUV_NV12, OutputFormat::YUV_NV12, 128, 128, ColorStandard::BT709}},
        
        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - PP Crop 测试（带分辨率）
        // ════════════════════════════════════════════════════════════════════
        // PP0 Crop
        {"pp0_720p_crop",           {"pp0", OutputFormat::YUV_NV12, 1280, 720, ColorStandard::BT709, 0, 0, 1280, 720}},
        {"pp0_1080p_crop",          {"pp0", OutputFormat::YUV_NV12, 1920, 1080, ColorStandard::BT709, 0, 0, 1920, 1080}},
        // PP1 RGB Crop
        {"pp1_720p_rgb_crop",       {"pp1", OutputFormat::RGB_RGB888, 1280, 720, ColorStandard::BT709, 0, 0, 1280, 720}},
        {"pp1_1080p_rgb_crop",      {"pp1", OutputFormat::RGB_RGB888, 1920, 1080, ColorStandard::BT709, 0, 0, 1920, 1080}},
        // PP1 YUV Crop
        {"pp1_720p_yuv_crop",       {"pp1", OutputFormat::YUV_NV12, 1280, 720, ColorStandard::BT709, 0, 0, 1280, 720}},
        {"pp1_1080p_yuv_crop",      {"pp1", OutputFormat::YUV_NV12, 1920, 1080, ColorStandard::BT709, 0, 0, 1920, 1080}},
        
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
        {"pp0_720p_nv12",           {"pp0", OutputFormat::YUV_NV12, 1280, 720}},
        {"pp0_720p_p010",           {"pp0", OutputFormat::YUV_P010, 1280, 720}},
        {"pp0_1080p_nv21",          {"pp0", OutputFormat::YUV_NV21, 1920, 1080}},
        {"pp0_4k_nv12",             {"pp0", OutputFormat::YUV_NV12, 3840, 2160}},
        
        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - PP1 带分辨率的格式测试
        // ════════════════════════════════════════════════════════════════════
        {"pp1_720p_argb8888",       {"pp1", OutputFormat::RGB_ARGB888, 1280, 720}},
        {"pp1_4k_argb8888",         {"pp1", OutputFormat::RGB_ARGB888, 3840, 2160}},
        {"pp1_720p_rgb888",         {"pp1", OutputFormat::RGB_RGB888, 1280, 720}},
        {"pp1_1080p_argb8888",      {"pp1", OutputFormat::RGB_ARGB888, 1920, 1080}},
        
        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - Crop 带输出分辨率
        // ════════════════════════════════════════════════════════════════════
        {"crop_720p_1024x576",      {"pp0", OutputFormat::YUV_NV12, 1024, 576, ColorStandard::BT709, 0, 0, 1280, 720}},
        {"crop_1080p_1600x900",     {"pp0", OutputFormat::YUV_NV12, 1600, 900, ColorStandard::BT709, 0, 0, 1920, 1080}},
        
        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - Scale 带输出分辨率
        // ════════════════════════════════════════════════════════════════════
        {"scale_720p_512x288",      {"pp0", OutputFormat::YUV_NV12, 512, 288, ColorStandard::BT709}},
        {"scale_1080p_800x450",     {"pp0", OutputFormat::YUV_NV12, 800, 450, ColorStandard::BT709}},
        
        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - H265 PP0/PP1 格式测试
        // ════════════════════════════════════════════════════════════════════
        {"pp0_h265_720p_nv12",      {"pp0", OutputFormat::YUV_NV12, 1280, 720}},
        {"pp0_h265_1080p_p010",     {"pp0", OutputFormat::YUV_P010, 1920, 1080}},
        {"pp0_h265_4k_nv12",        {"pp0", OutputFormat::YUV_NV12, 3840, 2160}},
        {"pp1_h265_720p_rgb888",    {"pp1", OutputFormat::RGB_RGB888, 1280, 720}},
        {"pp1_h265_1080p_argb8888", {"pp1", OutputFormat::RGB_ARGB888, 1920, 1080}},
        
        // ════════════════════════════════════════════════════════════════════
        // ZYW 新增测试 - H265 Crop+Scale
        // ════════════════════════════════════════════════════════════════════
        {"h265_1080p_crop_scale",   {"pp0", OutputFormat::YUV_NV12, 960, 540, ColorStandard::BT709, 0, 0, 1920, 1080}},
    };
    return tests;
}

std::vector<std::string> PPTestSuite::getTestNames() const {
    std::vector<std::string> names;
    for (const auto& pair : getPredefinedTests()) {
        names.push_back(pair.first);
    }
    return names;
}

// ========================================
// ITestModule 接口实现
// ========================================

int PPTestSuite::run(int argc, char* argv[]) {
    WorkerConfig config;
    PPTestParams params;
    
    if (!parseArgs(argc, argv, config, params)) {
        return 1;
    }
    
    // 构建消费标志
    uint32_t flags = consumer::CONSUME_COUNT | consumer::CONSUME_SAVE_RAW;
    if (config.consumer.enable_display) {
        flags |= consumer::CONSUME_DISPLAY;
    }
    
    // 使用 runSingle（ExecuteMode::SINGLE）
    auto result = runSingle(config.data_source.path, params, flags);
    
    std::string fmt_name(TacoConfigBuilder::mapFormatEnumToName(params.format));
    std::string test_name = params.channel + " " + fmt_name;
    
    consumer::BufferConsumerService::printResult(test_name, result);
    
    return result.success ? 0 : 1;
}

bool PPTestSuite::parseArgs(int argc, char* argv[], WorkerConfig& config, PPTestParams& params) {
    optind = 1;
    
    static struct option long_options[] = {
        {"help",       no_argument,       0, 'h'},
        {"list",       no_argument,       0, 'l'},
        {"input",      required_argument, 0, 'i'},
        {"format",     required_argument, 0, 'f'},
        {"channel",    required_argument, 0, 'c'},
        {"width",      required_argument, 0, 'W'},
        {"height",     required_argument, 0, 'H'},
        {"resolution", required_argument, 0, 'R'},
        {"crop",       required_argument, 0, 'C'},
        {"color-std",  required_argument, 0, 's'},
        {"output",     required_argument, 0, 'o'},
        {"save",       required_argument, 0, 'n'},
        {"display",    no_argument,       0, 'd'},
        {"max-frames", required_argument, 0, 'm'},
        {"verbose",    no_argument,       0, 'v'},
        {0, 0, 0, 0}
    };
    
    std::string input_path;
    std::string format_str = "nv12";
    std::string color_std_str = "bt601";
    
    int opt;
    while ((opt = getopt_long(argc, argv, "hli:f:c:W:H:R:C:s:o:n:dm:v",
                              long_options, nullptr)) != -1) {
        switch (opt) {
            case 'h':
                printHelp();
                return false;
            
            case 'l':
                listTests();
                return false;
            
            case 'i':
                input_path = optarg;
                break;
            
            case 'f':
                format_str = optarg;
                break;
            
            case 'c':
                params.channel = optarg;
                break;
            
            case 'W':
                params.width = std::stoi(optarg);
                break;
            
            case 'H':
                params.height = std::stoi(optarg);
                break;
            
            case 'R': {
                std::string res = optarg;
                size_t pos = res.find('x');
                if (pos != std::string::npos) {
                    params.width = std::stoi(res.substr(0, pos));
                    params.height = std::stoi(res.substr(pos + 1));
                }
                break;
            }
            
            case 'C': {
                // 解析裁剪参数: x,y,w,h
                std::string crop = optarg;
                int vals[4] = {0};
                int idx = 0;
                size_t start = 0, end;
                while ((end = crop.find(',', start)) != std::string::npos && idx < 4) {
                    vals[idx++] = std::stoi(crop.substr(start, end - start));
                    start = end + 1;
                }
                if (idx < 4 && start < crop.length()) {
                    vals[idx] = std::stoi(crop.substr(start));
                }
                params.crop_x = vals[0];
                params.crop_y = vals[1];
                params.crop_w = vals[2];
                params.crop_h = vals[3];
                break;
            }
            
            case 's':
                color_std_str = optarg;
                break;
            
            case 'o':
                config.consumer.output_path = optarg;
                break;
            
            case 'n':
                config.consumer.save_frames = std::stoi(optarg);
                break;
            
            case 'd':
                config.consumer.enable_display = true;
                break;
            
            case 'm':
                config.consumer.max_frames = std::stoi(optarg);
                break;
            
            case 'v':
                config.consumer.verbose = true;
                break;
            
            default:
                printHelp();
                return false;
        }
    }
    
    // 处理剩余参数
    for (int i = optind; i < argc; i++) {
        std::string arg = argv[i];
        
        // 检查是否是预定义测试
        const auto& tests = getPredefinedTests();
        auto it = tests.find(arg);
        if (it != tests.end()) {
            params = it->second;
            if (i + 1 < argc) {
                input_path = argv[++i];
            }
            continue;
        }
        
        if (input_path.empty()) {
            input_path = arg;
        }
    }
    
    if (input_path.empty()) {
        LOG4CPLUS_ERROR(getLogger(), "No input file specified");
        printHelp();
        return false;
    }
    
    config.data_source.path = input_path;
    
    // 只有当没有使用预定义测试时才解析 format 和 color_std 参数
    // 预定义测试通过 channel 非空来标识
    if (params.channel.empty()) {
        // 没有使用预定义测试，从命令行参数解析
        params.channel = "pp0";  // 默认 pp0
        params.format = TacoConfigBuilder::mapFormatNameToEnum(format_str);
        params.color_std = TacoConfigBuilder::mapColorStdNameToEnum(color_std_str);
    }
    // 如果使用了预定义测试，params 已经被正确设置，不需要覆盖
    
    return true;
}

int PPTestSuite::runPredefinedTest(const std::string& test_name, const std::string& path) {
    const auto& tests = getPredefinedTests();
    auto it = tests.find(test_name);
    if (it == tests.end()) {
        LOG4CPLUS_ERROR_FMT(getLogger(), "Unknown test '%s'", test_name.c_str());
        return 1;
    }
    
    // 使用 runSingle（ExecuteMode::SINGLE）
    auto result = runSingle(path, it->second, consumer::CONSUME_COUNT | consumer::CONSUME_SAVE_RAW);
    consumer::BufferConsumerService::printResult(test_name, result);
    return result.success ? 0 : 1;
}

void PPTestSuite::printHelp() const {
    std::cout << "\n";
    std::cout << "PP Module - 后处理格式测试\n";
    std::cout << "\n";
    std::cout << "Usage:\n";
    std::cout << "  qa_cases pp [options] <video_path>\n";
    std::cout << "  qa_cases pp [options] <test_name> <video_path>\n";
    std::cout << "\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --help              显示帮助信息\n";
    std::cout << "  -l, --list              列出所有预定义测试\n";
    std::cout << "  -i, --input <path>      输入视频路径\n";
    std::cout << "  -f, --format <fmt>      输出格式 (nv12|argb888|...)\n";
    std::cout << "  -c, --channel <ch>      通道选择 (pp0|pp1|multi)\n";
    std::cout << "  -W, --width <n>         输出宽度\n";
    std::cout << "  -H, --height <n>        输出高度\n";
    std::cout << "  -R, --resolution <WxH>  分辨率 (如 1920x1080)\n";
    std::cout << "  -C, --crop <x,y,w,h>    裁剪区域\n";
    std::cout << "  -s, --color-std <s>     颜色标准 (bt601|bt709|bt2020)\n";
    std::cout << "  -o, --output <path>     输出文件路径\n";
    std::cout << "  -n, --save <n>          保存帧数 (0=不保存, -1=全部)\n";
    std::cout << "  -d, --display           启用显示输出 (输出到 framebuffer)\n";
    std::cout << "  -m, --max-frames <n>    最大帧数 (-1=无限制)\n";
    std::cout << "  -v, --verbose           详细日志\n";
    std::cout << "\n";
    std::cout << "Supported formats:\n";
    std::cout << "  PP0 (YUV):  nv12, nv21, i420, yv12, p010, nv16, nv61, i422, nv24, i444\n";
    std::cout << "  PP1 (RGB):  argb888, abgr888, rgba888, bgra888, rgb888, bgr888\n";
    std::cout << "              xrgb888, xbgr888, rgbx888, bgrx888\n";
    std::cout << "              rgb888_planar, bgr888_planar, r16g16b16, b16g16r16, gbrp\n";
    std::cout << "  PP1 (YUV):  同 PP0\n";
    std::cout << "\n";
    std::cout << "Examples:\n";
    std::cout << "  qa_cases pp video.mp4\n";
    std::cout << "  qa_cases pp --format nv12 --channel pp0 video.mp4\n";
    std::cout << "  qa_cases pp --format argb888 --channel pp1 video.mp4\n";
    std::cout << "  qa_cases pp pp1_argb888 video.mp4\n";
    std::cout << "  qa_cases pp --crop 0,0,1280,720 --resolution 1280x720 video.mp4\n";
    std::cout << "\n";
}

void PPTestSuite::listTests() const {
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

// ========================================
// 核心测试方法实现（与 ExecuteMode 对齐）
// ========================================

WorkerConfig PPTestSuite::buildConfig(const std::string& path, const PPTestParams& params) {
    WorkerConfig config;
    
    if (params.channel == "pp0") {
        config = common::WorkerConfigFactory::createPP0YuvConfig(
            path, params.format, params.width, params.height, params.color_std);
    } else if (params.channel == "pp1") {
        int fmt_val = static_cast<int>(params.format);
        if (fmt_val >= 1000) {
            config = common::WorkerConfigFactory::createPP1RgbConfig(
                path, params.format, params.width, params.height, params.color_std);
        } else {
            config = common::WorkerConfigFactory::createPP1YuvConfig(
                path, params.format, params.width, params.height, params.color_std);
        }
    } else if (params.channel == "multi") {
        config = common::WorkerConfigFactory::createMultiPPConfig(
            path, params.format, params.pp1_format, 
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

TestResult PPTestSuite::runSingle(
    const std::string& path,
    const PPTestParams& params,
    uint32_t flags
) {
    auto& logger = getLogger();
    
    // 构建配置
    WorkerConfig config = buildConfig(path, params);
    config.consumer.save_frames = 10;  // 默认保存前10帧验证
    
    // 生成测试名称
    std::string fmt_name(TacoConfigBuilder::mapFormatEnumToName(params.format));
    std::string test_name = params.channel + " " + fmt_name;
    
    consumer::BufferConsumerService::printHeader(test_name, config);
    
    LOG4CPLUS_DEBUG_FMT(logger, "runSingle: mode=SINGLE, flags=0x%X, channel=%s", 
                        flags, params.channel.c_str());
    
    // 使用 BufferConsumerService，ExecuteMode::SINGLE
    consumer::BufferConsumerService service;
    return service.start({config}, consumer::ExecuteMode::SINGLE, flags);
}

} // namespace pp
} // namespace test
