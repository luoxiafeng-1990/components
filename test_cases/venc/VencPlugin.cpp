/**
 * @file VencPlugin.cpp
 * @brief 编码测试插件（预定义用例与备份分支 VencTestSuite::buildConfig 对齐）
 */

#include "VencPlugin.hpp"

#include "../common/third_party/CLI11.hpp"
#include "../common/WorkerConfigFactory.hpp"

#include "consumptionline/BufferConsumerService.hpp"
#include "productionline/MultiWorkerProductionLine.hpp"
#include "productionline/WorkerSyncCoordinator.hpp"
#include "productionline/worker/RawFrameSourceFromFile.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include "buffer/bufferpool/Buffer.hpp"

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/imgutils.h>
#include <libavutil/frame.h>
#include <libavutil/error.h>
}

#include <cstring>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <functional>

#include <chrono>
#include <thread>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

namespace test {
namespace venc {

namespace {

/** §2.1 H.264：10 种输入像素格式（与规格表一致，不含 BGR565） */
struct SpecFmt {
    const char* key;
    const char* input_format;
};
static const SpecFmt kSpec21H264Formats[] = {
    {"yuv420p", "yuv420p"},
    {"nv12", "nv12"},
    {"nv21", "nv21"},
    {"yuyv422", "yuyv422"},
    {"uyvy422", "uyvy422"},
    {"rgb444", "rgb888"},
    {"bgr444", "bgr888"},
    {"rgb555", "rgb555"},
    {"bgr555", "bgr555"},
    {"rgb565", "rgb565"},
};

/** §2.2 JPEG：在 §2.1 基础上增加 BGR565 / RGB888 / BGR888 / 10bit RGB */
static const SpecFmt kSpec22JpegExtraFormats[] = {
    {"bgr565", "bgr565"},
    {"rgb888", "rgb888"},
    {"bgr888", "bgr888"},
    {"rgb101010", "rgb101010"},
    {"bgr101010", "bgr101010"},
};

struct SpecRes {
    int w;
    int h;
    const char* tag;
};

static const SpecRes kSpec21Resolutions[] = {
    {144, 96, "144x96"},
    {320, 240, "320x240"},
    {640, 480, "640x480"},
    {1280, 720, "1280x720"},
    {1920, 1080, "1920x1080"},
};

/** H.265 在 §2.1 分辨率之外增加 4K（规格与产品矩阵常见要求） */
static const SpecRes kSpecH265Extra4K[] = {
    {3840, 2160, "3840x2160"},
};

static const SpecRes kSpec22JpegResolutions[] = {
    {96, 32, "96x32"},
    {512, 512, "512x512"},
    {1280, 720, "1280x720"},
    {1920, 1080, "1920x1080"},
    {3840, 2160, "3840x2160"},
    {8192, 8192, "8192x8192"},
};

static int matrixBitrateKbps(int w, int h) {
    const int64_t pixels = static_cast<int64_t>(w) * h;
    if (pixels <= 144 * 96)
        return 1200;
    if (pixels <= 320 * 240)
        return 1800;
    if (pixels <= 640 * 480)
        return 2500;
    if (pixels <= 1280 * 720)
        return 4000;
    if (pixels <= 1920 * 1080)
        return 6000;
    if (pixels <= 3840 * 2160)
        return 14000;
    return 28000;
}

} // namespace

int VencPlugin::parsePixelFormat(const std::string& format_str) {
    if (format_str == "nv12" || format_str == "NV12")
        return AV_PIX_FMT_NV12;
    if (format_str == "nv21" || format_str == "NV21")
        return AV_PIX_FMT_NV21;
    if (format_str == "yuv420p" || format_str == "YUV420P")
        return AV_PIX_FMT_YUV420P;
    if (format_str == "yuvj420p" || format_str == "YUVJ420P")
        return AV_PIX_FMT_YUVJ420P;
    if (format_str == "yuyv422" || format_str == "YUYV422" || format_str == "yuyv")
        return AV_PIX_FMT_YUYV422;
    if (format_str == "yvyu" || format_str == "YVYU")
        return AV_PIX_FMT_YVYU422;
    if (format_str == "uyvy" || format_str == "UYVY")
        return AV_PIX_FMT_UYVY422;
    if (format_str == "rgb24" || format_str == "RGB24" || format_str == "rgb888")
        return AV_PIX_FMT_RGB24;
    if (format_str == "bgr24" || format_str == "BGR24" || format_str == "bgr888")
        return AV_PIX_FMT_BGR24;
    if (format_str == "argb" || format_str == "ARGB")
        return AV_PIX_FMT_ARGB;
    if (format_str == "bgra" || format_str == "BGRA")
        return AV_PIX_FMT_BGRA;
    if (format_str == "rgba" || format_str == "RGBA")
        return AV_PIX_FMT_RGBA;
    if (format_str == "abgr" || format_str == "ABGR")
        return AV_PIX_FMT_ABGR;
    if (format_str == "rgb0" || format_str == "RGB0" || format_str == "rgbx888")
        return AV_PIX_FMT_RGB0;
    if (format_str == "bgr0" || format_str == "BGR0" || format_str == "bgrx888")
        return AV_PIX_FMT_BGR0;
    if (format_str == "rgb565" || format_str == "RGB565")
        return AV_PIX_FMT_RGB565LE;
    if (format_str == "bgr565" || format_str == "BGR565")
        return AV_PIX_FMT_BGR565LE;
    if (format_str == "rgb555" || format_str == "RGB555")
        return AV_PIX_FMT_RGB555LE;
    if (format_str == "bgr555" || format_str == "BGR555")
        return AV_PIX_FMT_BGR555LE;
#if defined(AV_PIX_FMT_X2RGB10LE)
    if (format_str == "x2rgb10le" || format_str == "rgbx101010" || format_str == "rgb101010")
        return AV_PIX_FMT_X2RGB10LE;
#endif
#if defined(AV_PIX_FMT_X2BGR10LE)
    if (format_str == "x2bgr10le" || format_str == "bgrx101010" || format_str == "bgr101010")
        return AV_PIX_FMT_X2BGR10LE;
#endif
    return AV_PIX_FMT_NV12;
}

static bool isParallelProfile(const std::string& pr) {
    return pr.size() >= 10 && pr.compare(0, 9, "parallel_") == 0;
}

WorkerConfig buildEncodeConfigInternal(const EncodeTestParams& params, const std::string& yuv_path) {
    using test::common::WorkerConfigFactory;

    const int output_width = params.output_width > 0 ? params.output_width : params.input_width;
    const int output_height = params.output_height > 0 ? params.output_height : params.input_height;
    const double fps = params.output_fps > 0 ? params.output_fps : params.input_fps;
    const int gop = params.gop_size > 0 ? params.gop_size : 30;
    const int pix = VencPlugin::parsePixelFormat(params.input_format);
    const int br_kbps = params.bitrate > 0 ? params.bitrate : 0;

    const std::string& c = params.codec;
    const bool is_jpeg = (c == "jpeg");
    const bool is_hevc = (c == "h265" || c == "hevc");
    const bool is_h264 = (c == "h264" || (!is_jpeg && !is_hevc)); // 未识别 codec 时回退 h264

    WorkerConfig config;

    if (is_jpeg) {
        const int q = params.jpeg_quality > 0 ? params.jpeg_quality : 80;
        const double jfps = fps > 0.0 ? fps : 25.0;
        config = WorkerConfigFactory::createJpegEncode(
            yuv_path, output_width, output_height, q, jfps, pix);
    } else if (is_hevc) {
        if (params.use_hardware) {
            int prof = 1;
            if (!isParallelProfile(params.profile) && params.profile == "main10")
                prof = 2;
            config = WorkerConfigFactory::createH265Encode(
                yuv_path, output_width, output_height, br_kbps, fps, gop, prof, pix);
            if (!isParallelProfile(params.profile)) {
                const int w = output_width;
                const int h = output_height;
                const double hfps = fps;
                if (w <= 1280 && h <= 720 && hfps <= 30.0)
                    config.encoder.taco.level = 120;
                else if (w <= 1920 && h <= 1080 && hfps <= 30.0)
                    config.encoder.taco.level = 150;
                else if (w <= 1920 && h <= 1080 && hfps <= 60.0)
                    config.encoder.taco.level = 153;
                else
                    config.encoder.taco.level = 150;
            }
        } else {
            config = WorkerConfigFactory::createSoftwareEncode(
                yuv_path, "h265", output_width, output_height, br_kbps, fps, gop, pix);
        }
    } else if (is_h264) {
        if (params.use_hardware) {
            int prof = 77;
            if (!isParallelProfile(params.profile)) {
                if (params.profile == "baseline")
                    prof = 66;
                else if (params.profile == "main")
                    prof = 77;
                else if (params.profile == "high")
                    prof = 100;
            }
            config = WorkerConfigFactory::createH264Encode(
                yuv_path, output_width, output_height, br_kbps, fps, gop, prof, pix);
        } else {
            config = WorkerConfigFactory::createSoftwareEncode(
                yuv_path, "h264", output_width, output_height, br_kbps, fps, gop, pix);
        }
    }

    if (isParallelProfile(params.profile)) {
        config.encoder.taco.profile = 0;
        config.encoder.taco.level = 0;
    }

    config.encoder.rc_mode = params.rc_mode;
    config.data_source.buffer_count = 8;
    config.data_source.buffer_mode = false;
    return config;
}

WorkerConfig VencPlugin::buildEncodeConfig(const EncodeTestParams& params) {
    return buildEncodeConfigInternal(params, input_path_);
}

const std::map<std::string, EncodeTestParams>& VencPlugin::getPredefinedTests() {
    static std::map<std::string, EncodeTestParams> tests;
    if (!tests.empty())
        return tests;

    auto add = [&](const char* name, EncodeTestParams p) {
        p.predefined_name = name;
        tests[name] = std::move(p);
    };

    add("h264_1280x720_30_4mbps", [&] {
        EncodeTestParams p("h264", "main", 4000, 30, true);
        p.input_format = "nv12";
        p.input_width = 1280;
        p.input_height = 720;
        p.input_fps = 30.0;
        return p;
    }());

    add("h264_1920x1080_30_4mbps", [&] {
        EncodeTestParams p("h264", "main", 4000, 30, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 30.0;
        return p;
    }());

    add("h264_1920x1080_30_8mbps", [&] {
        EncodeTestParams p("h264", "high", 8000, 30, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 30.0;
        return p;
    }());

    add("h264_1920x1080_60_8mbps", [&] {
        EncodeTestParams p("h264", "high", 8000, 30, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 60.0;
        return p;
    }());

    add("h264_3840x2160_30_16mbps", [&] {
        EncodeTestParams p("h264", "high", 16000, 30, true);
        p.input_format = "nv12";
        p.input_width = 3840;
        p.input_height = 2160;
        p.input_fps = 30.0;
        return p;
    }());

    add("h265_1920x1080_30_4mbps", [&] {
        EncodeTestParams p("h265", "main", 4000, 30, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 30.0;
        return p;
    }());

    add("h264_1920x1080_30_4mbps_gop1", [&] {
        EncodeTestParams p("h264", "main", 4000, 1, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 30.0;
        return p;
    }());

    add("h264_1920x1080_30_4mbps_gop120", [&] {
        EncodeTestParams p("h264", "main", 4000, 120, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 30.0;
        return p;
    }());

    add("h264_1920x1080_30_cqp", [&] {
        EncodeTestParams p("h264", "main", 0, 30, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 30.0;
        p.rc_mode = 2;
        return p;
    }());

    add("h264_1920x1080_30_4mbps_cbr", [&] {
        EncodeTestParams p("h264", "main", 4000, 30, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 30.0;
        p.rc_mode = 0;
        return p;
    }());

    add("jpeg_1920x1080_q80", [&] {
        EncodeTestParams p("jpeg", "", 0, 0, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 25.0;
        p.jpeg_quality = 80;
        return p;
    }());

    add("h264_640x480_30", [&] {
        EncodeTestParams p("h264", "main", 2000, 30, true);
        p.input_format = "nv12";
        p.input_width = 640;
        p.input_height = 480;
        p.input_fps = 30.0;
        return p;
    }());

    add("venc_parallel_2ch", [&] {
        EncodeTestParams p("h264", "parallel_2", 4000, 30, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 30.0;
        return p;
    }());

    add("venc_parallel_4ch", [&] {
        EncodeTestParams p("h264", "parallel_4", 4000, 30, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 30.0;
        return p;
    }());

    add("venc_parallel_8ch", [&] {
        EncodeTestParams p("h264", "parallel_8", 4000, 30, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 30.0;
        return p;
    }());

    add("sw_h264_1920x1080_30", [&] {
        EncodeTestParams p("h264", "main", 4000, 30, false);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 30.0;
        return p;
    }());

    add("sw_h265_1920x1080_30", [&] {
        EncodeTestParams p("h265", "main", 4000, 30, false);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 30.0;
        return p;
    }());

    // ── 与备份 VencTestSuite 对齐的补充用例（码率/Profile/GOP/RC/并行等）──
    add("h265_1280x720_30_2mbps", [&] {
        EncodeTestParams p("h265", "main", 2000, 30, true);
        p.input_format = "nv12";
        p.input_width = 1280;
        p.input_height = 720;
        p.input_fps = 30.0;
        return p;
    }());
    add("h265_1920x1080_30_8mbps", [&] {
        EncodeTestParams p("h265", "main", 8000, 30, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 30.0;
        return p;
    }());
    add("h265_3840x2160_30_12mbps", [&] {
        EncodeTestParams p("h265", "main", 12000, 30, true);
        p.input_format = "nv12";
        p.input_width = 3840;
        p.input_height = 2160;
        p.input_fps = 30.0;
        return p;
    }());
    add("jpeg_1920x1080_q90", [&] {
        EncodeTestParams p("jpeg", "", 0, 0, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 25.0;
        p.jpeg_quality = 90;
        return p;
    }());
    add("h264_1920x1080_30_4mbps_gop15", [&] {
        EncodeTestParams p("h264", "main", 4000, 15, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 30.0;
        return p;
    }());
    add("h264_1920x1080_30_4mbps_gop60", [&] {
        EncodeTestParams p("h264", "main", 4000, 60, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 30.0;
        return p;
    }());
    add("h264_1920x1080_30_4mbps_vbr", [&] {
        EncodeTestParams p("h264", "main", 4000, 30, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 30.0;
        p.rc_mode = 1;
        return p;
    }());
    add("h265_1920x1080_30_4mbps_main10", [&] {
        EncodeTestParams p("h265", "main10", 4000, 30, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 30.0;
        return p;
    }());
    add("h264_2560x1440_30_6mbps", [&] {
        EncodeTestParams p("h264", "main", 6000, 30, true);
        p.input_format = "nv12";
        p.input_width = 2560;
        p.input_height = 1440;
        p.input_fps = 30.0;
        return p;
    }());
    add("h265_2560x1440_30_5mbps", [&] {
        EncodeTestParams p("h265", "main", 5000, 30, true);
        p.input_format = "nv12";
        p.input_width = 2560;
        p.input_height = 1440;
        p.input_fps = 30.0;
        return p;
    }());
    add("venc_parallel_16ch", [&] {
        EncodeTestParams p("h264", "parallel_16", 4000, 30, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 30.0;
        return p;
    }());
    add("venc_parallel_32ch_720p", [&] {
        EncodeTestParams p("h264", "parallel_32", 2000, 30, true);
        p.input_format = "nv12";
        p.input_width = 1280;
        p.input_height = 720;
        p.input_fps = 30.0;
        return p;
    }());

    // ── 规格矩阵：§2.1 编码 H.264（10 格式 × 5 分辨率 = 50）──
    for (const auto& f : kSpec21H264Formats) {
        for (const auto& r : kSpec21Resolutions) {
            const int br = matrixBitrateKbps(r.w, r.h);
            std::string name = std::string("spec21_h264_") + f.key + "_" + r.tag;
            EncodeTestParams p("h264", "main", br, 30, true);
            p.input_format = f.input_format;
            p.input_width = r.w;
            p.input_height = r.h;
            p.input_fps = 30.0;
            add(name.c_str(), std::move(p));
        }
    }

    // ── §2.1 扩展：H.265 同像素格式矩阵 + 4K（10×5 + 10×1）──
    for (const auto& f : kSpec21H264Formats) {
        for (const auto& r : kSpec21Resolutions) {
            const int br = matrixBitrateKbps(r.w, r.h);
            std::string name = std::string("spec21_h265_") + f.key + "_" + r.tag;
            EncodeTestParams p("h265", "main", br, 30, true);
            p.input_format = f.input_format;
            p.input_width = r.w;
            p.input_height = r.h;
            p.input_fps = 30.0;
            add(name.c_str(), std::move(p));
        }
    }
    for (const auto& f : kSpec21H264Formats) {
        for (const auto& r : kSpecH265Extra4K) {
            const int br = matrixBitrateKbps(r.w, r.h);
            std::string name = std::string("spec21_h265_") + f.key + "_" + r.tag;
            EncodeTestParams p("h265", "main", br, 30, true);
            p.input_format = f.input_format;
            p.input_width = r.w;
            p.input_height = r.h;
            p.input_fps = 30.0;
            add(name.c_str(), std::move(p));
        }
    }

    // ── §2.2 编码 JPEG（15 格式 × 6 分辨率 = 90）──
    auto addJpegMatrixRow = [&](const SpecFmt& f) {
        for (const auto& r : kSpec22JpegResolutions) {
            std::string name = std::string("spec22_jpeg_") + f.key + "_" + r.tag;
            EncodeTestParams p("jpeg", "", 0, 0, true);
            p.input_format = f.input_format;
            p.input_width = r.w;
            p.input_height = r.h;
            p.input_fps = 25.0;
            p.jpeg_quality = 80;
            add(name.c_str(), std::move(p));
        }
    };
    for (const auto& f : kSpec21H264Formats)
        addJpegMatrixRow(f);
    for (const auto& f : kSpec22JpegExtraFormats)
        addJpegMatrixRow(f);

    // ── §2.4 / 大图缩放编码：输入 8192×8192，输出小于输入（需对应尺寸源文件；旋转/裁剪由硬件 PP 时需另行用例）──
    auto addScale8192 = [&](const char* codec, const char* suffix, int out_w, int out_h, int br) {
        std::string name =
            std::string("spec24_") + codec + "_8192in_scale_" + suffix;
        const bool is_jpeg = (std::strcmp(codec, "jpeg") == 0);
        EncodeTestParams p(codec, is_jpeg ? "" : "main", br, 30, true);
        p.input_format = "nv12";
        p.input_width = 8192;
        p.input_height = 8192;
        p.input_fps = 25.0;
        p.output_width = out_w;
        p.output_height = out_h;
        if (is_jpeg) {
            p.gop_size = 0;
            p.jpeg_quality = 85;
        }
        add(name.c_str(), std::move(p));
    };
    addScale8192("h264", "144x96", 144, 96, 28000);
    addScale8192("h264", "1280x720", 1280, 720, 28000);
    addScale8192("h264", "1920x1080", 1920, 1080, 28000);
    addScale8192("h264", "2560x1440", 2560, 1440, 28000);
    addScale8192("h265", "1920x1080", 1920, 1080, 28000);
    addScale8192("h265", "1280x720", 1280, 720, 28000);
    addScale8192("jpeg", "96x32", 96, 32, 0);
    addScale8192("jpeg", "1280x720", 1280, 720, 0);
    addScale8192("jpeg", "1920x1080", 1920, 1080, 0);

    // ── §2.5 / §2.7：多通道与性能表常见组合（128 路需环境与显存足够）──
    add("venc_parallel_128ch_1080p", [&] {
        EncodeTestParams p("h264", "parallel_128", 4000, 30, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 25.0;
        return p;
    }());
    add("venc_parallel_8ch_mjpeg_720p", [&] {
        EncodeTestParams p("jpeg", "parallel_8", 0, 0, true);
        p.input_format = "nv12";
        p.input_width = 1280;
        p.input_height = 720;
        p.input_fps = 25.0;
        p.jpeg_quality = 80;
        return p;
    }());
    add("venc_parallel_16ch_mjpeg_8192", [&] {
        EncodeTestParams p("jpeg", "parallel_16", 0, 0, true);
        p.input_format = "nv12";
        p.input_width = 8192;
        p.input_height = 8192;
        p.input_fps = 25.0;
        p.jpeg_quality = 80;
        return p;
    }());

    // §2.7：25fps × 多输入格式（单路，便于与性能表对照）
    add("spec27_h264_1080p_25fps_yuv420p", [&] {
        EncodeTestParams p("h264", "main", 5000, 30, true);
        p.input_format = "yuv420p";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 25.0;
        return p;
    }());
    add("spec27_h264_1080p_25fps_nv12", [&] {
        EncodeTestParams p("h264", "main", 5000, 30, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 25.0;
        return p;
    }());
    add("spec27_h264_1080p_25fps_yuyv422", [&] {
        EncodeTestParams p("h264", "main", 5000, 30, true);
        p.input_format = "yuyv422";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 25.0;
        return p;
    }());
    add("spec27_h264_1080p_25fps_rgb888", [&] {
        EncodeTestParams p("h264", "main", 5000, 30, true);
        p.input_format = "rgb888";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 25.0;
        return p;
    }());

    return tests;
}

EncodeTestParams VencPlugin::resolveParams() const {
    EncodeTestParams p = params_;
    if (threads_ > 0)
        p.profile = "parallel_" + std::to_string(threads_);
    return p;
}

void VencPlugin::registerOptions(CLI::App& app) {
    app.add_flag("-l,--list", show_list_, "列出预定义编码测试");
    app.add_option("-i,--input", input_path_, "YUV/RGB 输入文件路径");
    app.add_option("-c,--codec", params_.codec, "编码格式 h264|h265|jpeg");
    app.add_option("-P,--profile", params_.profile, "Profile (baseline|main|high|main10)");
    app.add_option("-b,--bitrate", params_.bitrate, "码率 (kbps)");
    app.add_option("-g,--gop", params_.gop_size, "GOP 大小");
    app.add_option("-W,--width", params_.input_width, "输入宽度");
    app.add_option("-H,--height", params_.input_height, "输入高度");
    app.add_option("-F,--fps", params_.input_fps, "输入/编码帧率");
    app.add_option("-f,--input-format", params_.input_format, "输入像素格式 (默认 nv12)");
    app.add_option("-m,--max-frames", max_frames_, "最大帧数 (-1=不限制)");
    app.add_flag("--loop", loop_, "循环读取输入");
    app.add_option("-o,--output", encoded_output_path_,
        "编码输出路径：推荐 .mp4 封装便于通用播放器/ vdec 回放；亦支持 .h264/.265/.mjpeg 等（扩展名决定复用器）");
    app.add_flag("-v,--verbose", verbose_, "详细日志");
    app.add_option("-t,--threads", threads_, "并行通道数 (PARALLEL 模式)");

    app.add_flag("-d,--display", enable_display_, "启用显示");
    app.add_option("--display-mode", display_mode_str_, "shared_fb | vo");
    app.add_option("--display-fps", display_fps_, "显示刷新帧率");
    app.add_flag("--osd", osd_enable_, "OSD");
    app.add_option("--osd-fps", osd_fps_, "OSD 刷新频率");

    app.add_flag("-p,--psnr", enable_psnr_, "启用 PSNR：单路编码时对比源 YUV 与 编码→软解 输出");
    app.add_flag("-S,--ssim", enable_ssim_, "启用 SSIM（同上）");
    app.add_option("-M,--min-psnr", min_psnr_, "PSNR 阈值 (dB)，与 stress 脚本 -M 一致");
    app.add_option("-N,--min-ssim", min_ssim_, "SSIM 阈值，与 stress 脚本 -N 一致");

    app.add_option("positional", positional_args_, "预定义测试名或输入文件");

    app.footer(
        "示例:\n"
        "  qa_cases venc -l\n"
        "  qa_cases venc h264_1920x1080_60_8mbps /data/in.nv12\n"
        "  qa_cases venc -p -S -M 38 -N 0.95 h264_1920x1080_60_8mbps /data/in.nv12\n"
        "  qa_cases venc -t 4 -o /tmp/o.mp4 h264_1920x1080_30_4mbps /data/in.nv12\n");
}

void VencPlugin::applyTo(WorkerConfig& config) const {
    if (!input_path_.empty())
        config.data_source.path = input_path_;
    config.data_source.max_frames = max_frames_;
    config.data_source.loop = loop_;
    config.consumer_type.verbose = verbose_;

    if (!encoded_output_path_.empty()) {
        config.consumer_type.save_encoded.enable = true;
        config.consumer_type.save_encoded.output_path = encoded_output_path_;
    }

    if (enable_display_) {
        using DisplayMode = WorkerConfig::ConsumerTypeConfig::DisplayType::DisplayMode;
        config.consumer_type.display.enable = true;
        if (display_mode_str_ == "vo" || display_mode_str_ == "taco-vo")
            config.consumer_type.display.mode = DisplayMode::TACO_VO;
        else
            config.consumer_type.display.mode = DisplayMode::SHARED_FB;
        config.consumer_type.display.taco_vo.target_fps = display_fps_;
        config.consumer_type.display.taco_vo.osd_enable = osd_enable_;
        config.consumer_type.display.taco_vo.osd_fps = osd_fps_;
    }

    config.consumer_type.compare.enable_psnr = enable_psnr_;
    config.consumer_type.compare.enable_ssim = enable_ssim_;
    if (min_psnr_ > 0.0) {
        config.consumer_type.compare.min_psnr = min_psnr_;
    }
    if (min_ssim_ > 0.0) {
        config.consumer_type.compare.min_ssim = min_ssim_;
    }
}

int VencPlugin::handlePreActions() {
    const auto& tmap = getPredefinedTests();
    for (const auto& arg : positional_args_) {
        auto it = tmap.find(arg);
        if (it != tmap.end()) {
            params_ = it->second;
            params_.predefined_name = it->first;
            continue;
        }
        if (input_path_.empty())
            input_path_ = arg;
    }

    if (show_list_) {
        listTests();
        return 0;
    }
    if (input_path_.empty()) {
        std::cerr << "Error: 请指定输入 YUV 文件，或使用 -i/--input\n";
        return 1;
    }
    return -1;
}

std::string VencPlugin::getTestName() const {
    auto p = resolveParams();
    std::ostringstream name;
    if (p.isPredefined()) {
        name << p.predefined_name << " (" << p.codec << " " << p.input_width << "x" << p.input_height
             << " @" << p.input_fps << "fps)";
    } else {
        name << "CustomEncode: " << p.codec << " " << p.input_width << "x" << p.input_height;
    }
    return name.str();
}

std::vector<WorkerConfig> VencPlugin::buildPipelineConfigs(const WorkerConfig& shared_config) {
    if (input_path_.empty())
        return {};

    auto p = resolveParams();

    if (p.profile == "parallel" || isParallelProfile(p.profile)) {
        int thread_count = 2;
        if (isParallelProfile(p.profile))
            thread_count = std::max(2, std::stoi(p.profile.substr(9)));

        WorkerConfig base = shared_config;
        if (base.consumer_type.display.enable &&
            (base.consumer_type.display.mode == WorkerConfig::ConsumerTypeConfig::DisplayType::TACO_VO ||
             base.consumer_type.display.mode == WorkerConfig::ConsumerTypeConfig::DisplayType::SHARED_FB)) {
            base.consumer_type.display.taco_vo.max_channels = thread_count;
        }

        std::vector<std::string> out_paths;
        if (base.consumer_type.save_encoded.enable && !base.consumer_type.save_encoded.output_path.empty()) {
            const std::string& first = base.consumer_type.save_encoded.output_path;
            size_t dot = first.rfind('.');
            if (dot == std::string::npos) {
                for (int i = 0; i < thread_count; i++)
                    out_paths.push_back(first + "_" + std::to_string(i + 1));
            } else {
                std::string b = first.substr(0, dot);
                std::string ext = first.substr(dot);
                for (int i = 0; i < thread_count; i++)
                    out_paths.push_back(b + "_" + std::to_string(i + 1) + ext);
            }
        }

        std::vector<WorkerConfig> configs;
        for (int i = 0; i < thread_count; i++) {
            WorkerConfig cfg = buildEncodeConfigInternal(p, shared_config.data_source.path);
            cfg.consumer_type = base.consumer_type;
            cfg.data_source.max_frames = base.data_source.max_frames;
            cfg.data_source.loop = base.data_source.loop;
            if (!out_paths.empty()) {
                cfg.consumer_type.save_encoded.enable = true;
                cfg.consumer_type.save_encoded.output_path =
                    (static_cast<size_t>(i) < out_paths.size()) ? out_paths[static_cast<size_t>(i)] : out_paths[0];
            }
            cfg.consumer_type.performance.target_fps = p.input_fps;
            configs.push_back(std::move(cfg));
        }
        return configs;
    }

    WorkerConfig full = buildEncodeConfigInternal(p, shared_config.data_source.path);
    full.consumer_type = shared_config.consumer_type;
    full.data_source.max_frames = shared_config.data_source.max_frames;
    full.data_source.loop = shared_config.data_source.loop;
    full.consumer_type.performance.target_fps = p.input_fps;
    return {full};
}

void VencPlugin::listTests() const {
    std::cout << "\n预定义 VENC 测试（含 §2.1 H.264 / §2.1+H.265+4K / §2.2 JPEG 矩阵、§2.4 类 8192 缩放、"
                 "§2.5/§2.7 并行与 25fps 等；矩阵用例需准备与 input 分辨率一致的源文件）:\n"
              << "────────────────────────────────────────\n";
    for (const auto& kv : getPredefinedTests()) {
        const auto& n = kv.first;
        const auto& q = kv.second;
        std::cout << "  " << n << "  [" << q.codec << " " << q.input_width << "x" << q.input_height;
        if (q.output_width > 0 && q.output_height > 0)
            std::cout << " -> " << q.output_width << "x" << q.output_height;
        std::cout << "]\n";
    }
    std::cout << "────────────────────────────────────────\n\n";
}

namespace {

log4cplus::Logger& getLogger() {
    static log4cplus::Logger logger = log4cplus::Logger::getInstance(
        LOG4CPLUS_TEXT("components.test.venc.EncodeCompare"));
    return logger;
}

consumer::ConsumeResult failResult(
    log4cplus::Logger& logger,
    const char* stage,
    const std::string& message)
{
    consumer::ConsumeResult r;
    r.success = false;
    r.error_message = message;
    LOG4CPLUS_ERROR_FMT(logger, "[%s] %s", stage, message.c_str());
    return r;
}

struct EncodeQualityCompareCallbackCtx {
    std::shared_ptr<CompareCallbackContext> compare_ctx;
    RawFrameSourceFromFile* ref_source = nullptr;
    AVFrame* ref_avframe = nullptr;
    Buffer* ref_buf_wrap = nullptr;

    // 仅用于调试/错误信息：ref 结束后不再做比较
    std::atomic<bool> ref_eof{false};
};

} // namespace

consumer::ConsumeResult runEncodeQualityCompare(
    const WorkerConfig& encode_cfg,
    const WorkerConfig& shared_cfg,
    const std::string& test_name)
{
    auto& logger = getLogger();
    consumer::ConsumeResult result;
    result.success = false;

    if (!test_name.empty()) {
        consumer::BufferConsumerService::printHeader(test_name, encode_cfg);
        LOG4CPLUS_INFO(logger, "");
        LOG4CPLUS_INFO(logger, "═══════════════════════════════════════════════════════");
        LOG4CPLUS_INFO(logger, "  Mode:       Encode -> decode -> Compare (via frame-sync callback)");
        if (!encode_cfg.data_source.path.empty()) {
            LOG4CPLUS_INFO_FMT(logger, "  Input:      %s", encode_cfg.data_source.path.c_str());
        }
        LOG4CPLUS_INFO(logger, "═══════════════════════════════════════════════════════");
    }

    const int ref_width = encode_cfg.display.width;
    const int ref_height = encode_cfg.display.height;
    const AVPixelFormat ref_pix_fmt =
        static_cast<AVPixelFormat>(encode_cfg.encoder.input_pix_fmt);

    // 1) 准备参考帧数据源
    RawFrameSourceFromFile ref_source(
        encode_cfg.data_source.path, ref_width, ref_height, ref_pix_fmt);
    if (!ref_source.open()) {
        return failResult(
            logger, "ENC_COMPARE", "Failed to open reference input file: " + encode_cfg.data_source.path);
    }

    AVFrame* ref_avframe = av_frame_alloc();
    if (!ref_avframe) {
        ref_source.close();
        return failResult(logger, "ENC_COMPARE", "Failed to allocate reference AVFrame");
    }
    ref_avframe->format = ref_pix_fmt;
    ref_avframe->width = ref_width;
    ref_avframe->height = ref_height;
    if (av_frame_get_buffer(ref_avframe, 0) < 0) {
        av_frame_free(&ref_avframe);
        ref_source.close();
        return failResult(logger, "ENC_COMPARE", "Failed to allocate buffer for reference AVFrame");
    }
    const int ref_buf_size = static_cast<int>(
        av_image_get_buffer_size(ref_pix_fmt, ref_width, ref_height, 1));

    // 先用 frame->data[0] 构造 Buffer wrapper；具体元数据在 callback 内逐帧刷新
    Buffer ref_buf_wrap(
        0, ref_avframe->data[0], 0, static_cast<size_t>(ref_buf_size),
        Buffer::Ownership::EXTERNAL);
    ref_buf_wrap.setAVFrame(ref_avframe);
    ref_buf_wrap.setImageMetadataFromAVFrame(ref_avframe);

    // 2) compare_ctx：沿用 WorkerSyncCoordinator 的 BufferComparator 累积统计
    auto compare_ctx = std::make_shared<CompareCallbackContext>();
    compare_ctx->initFromCompareType(shared_cfg.consumer_type.compare);
    if (!compare_ctx->openComparator()) {
        av_frame_free(&ref_avframe);
        ref_source.close();
        return failResult(logger, "ENC_COMPARE", "Failed to open BufferComparator");
    }

    EncodeQualityCompareCallbackCtx cb_ctx;
    cb_ctx.compare_ctx = compare_ctx;
    cb_ctx.ref_source = &ref_source;
    cb_ctx.ref_avframe = ref_avframe;
    cb_ctx.ref_buf_wrap = &ref_buf_wrap;

    // 3) MultiWorker：encoder -> decoder，并用 callback 做 compare
    MultiWorkerConfig multi_config;
    WorkerGroupConfig group("encode_decode_quality_compare_group");

    ProducerConfig encode_producer;
    encode_producer.producer_name = "encoder";
    encode_producer.worker_config = encode_cfg;
    encode_producer.worker_config.worker_type = WorkerType::FFMPEG_ENCODE;
    // 质量验证路径不需要显示，避免显示侧副作用
    encode_producer.worker_config.consumer_type.display.enable = false;
    group.producer_configs.push_back(encode_producer);

    ConsumerConfig decode_consumer;
    decode_consumer.consumer_name = "decoder";
    decode_consumer.worker_config = encode_cfg;
    decode_consumer.worker_config.worker_type = WorkerType::FFMPEG_DECODE;
    decode_consumer.worker_config.data_source.buffer_mode = true;
    decode_consumer.worker_config.decoder.name = std::nullopt;
    decode_consumer.worker_config.decoder.enable_hardware = false;  // 软件解码用于与参考对比
    decode_consumer.worker_config.consumer_type.display.enable = false;
    // 此 worker 不做“compare 消费策略”，compare 在 callback 里完成
    decode_consumer.worker_config.consumer_type.compare.enable_psnr = false;
    decode_consumer.worker_config.consumer_type.compare.enable_ssim = false;
    group.consumer_configs.push_back(decode_consumer);

    ConnectorConfig connector;
    connector.mode = Connector::Mode::ONE_TO_ONE;
    connector.producer_names = {"encoder"};
    connector.consumer_names = {"decoder"};
    connector.enable_frame_sync = true;

    FrameSyncCallback callback = [](uint64_t /*frame_version*/,
                                     const std::map<std::string, Buffer*>& worker_buffers,
                                     void* ctx) -> bool {
        auto* cb = static_cast<EncodeQualityCompareCallbackCtx*>(ctx);
        if (!cb || !cb->compare_ctx || !cb->ref_source || !cb->ref_avframe || !cb->ref_buf_wrap) {
            return true;
        }
        if (cb->ref_eof.load(std::memory_order_acquire)) {
            return true;
        }
        if (worker_buffers.empty()) {
            return true;
        }
        Buffer* decoded_buf = worker_buffers.begin()->second;
        if (!decoded_buf) {
            return true;
        }
        int ref_ret = cb->ref_source->readRawFrame(cb->ref_avframe);
        if (ref_ret == AVERROR_EOF || ref_ret < 0) {
            cb->ref_eof.store(true, std::memory_order_release);
            return true;
        }

        cb->ref_buf_wrap->setAVFrame(cb->ref_avframe);
        cb->ref_buf_wrap->setImageMetadataFromAVFrame(cb->ref_avframe);

        // comparator_ 在主线程已 open
        auto cmp = cb->compare_ctx->comparator_->compare(cb->ref_buf_wrap, decoded_buf);
        const double psnr = cmp.psnr_avg;
        const double ssim = cmp.ssim_avg;

        const bool psnr_ok = !cb->compare_ctx->enable_psnr || (psnr >= cb->compare_ctx->min_psnr);
        const bool ssim_ok = !cb->compare_ctx->enable_ssim || (ssim >= cb->compare_ctx->min_ssim);
        const bool passed = psnr_ok && ssim_ok;

        cb->compare_ctx->total_frames.fetch_add(1, std::memory_order_relaxed);
        if (passed) {
            cb->compare_ctx->passed_frames.fetch_add(1, std::memory_order_relaxed);
        } else {
            cb->compare_ctx->failed_frames.fetch_add(1, std::memory_order_relaxed);
        }

        double old_psnr = cb->compare_ctx->psnr_sum.load(std::memory_order_relaxed);
        while (!cb->compare_ctx->psnr_sum.compare_exchange_weak(
            old_psnr, old_psnr + psnr, std::memory_order_relaxed)) {}

        double old_ssim = cb->compare_ctx->ssim_sum.load(std::memory_order_relaxed);
        while (!cb->compare_ctx->ssim_sum.compare_exchange_weak(
            old_ssim, old_ssim + ssim, std::memory_order_relaxed)) {}

        if (cb->compare_ctx->verbose && !passed) {
            LOG4CPLUS_WARN_FMT(
                log4cplus::Logger::getRoot(),
                "Encode-compare frame FAILED: PSNR=%.2f (min=%.2f), SSIM=%.4f (min=%.4f)",
                psnr, cb->compare_ctx->min_psnr, ssim, cb->compare_ctx->min_ssim);
        }
        return true;
    };

    connector.callback_chain.push_back(
        WorkerSyncCoordinator::createDefaultCompareCallback(compare_ctx.get()) );
    // 用我们自己的 compare 逻辑覆盖 default_compare_callback 的工作：
    // - default callback 需要 reference/test 两路 buffer
    // - 本场景只有 decoder 输出，所以我们改为“只注册一次自己的回调”
    connector.callback_chain.clear();
    connector.callback_chain.push_back(
        CallbackChainItem(callback, &cb_ctx, "venc_encode_quality_callback"));

    group.connector_configs.push_back(connector);
    multi_config.groups.push_back(group);

    auto start_time = std::chrono::steady_clock::now();
    std::unique_ptr<MultiWorkerProductionLine> pipeline;
    bool pipeline_started = false;

    auto cleanup = [&]() {
        if (pipeline_started && pipeline) {
            pipeline->stop();
            pipeline_started = false;
        }
    };
    struct ScopeExit {
        std::function<void()> fn;
        ~ScopeExit() { if (fn) fn(); }
    } scope_exit{cleanup};

    pipeline = std::make_unique<MultiWorkerProductionLine>(multi_config, encode_cfg.data_source.loop, 1, false);
    if (!pipeline->start()) {
        av_frame_free(&ref_avframe);
        ref_source.close();
        return failResult(logger, "ENC_COMPARE", "Failed to start encode-decode pipeline");
    }
    pipeline_started = true;

    const uint64_t decoded_pool_id = pipeline->getGroupConsumerBufferPoolId(0, 0);
    auto decoded_pool = BufferPoolRegistry::getInstance().getPool(decoded_pool_id).lock();
    if (!decoded_pool) {
        av_frame_free(&ref_avframe);
        ref_source.close();
        return failResult(logger, "ENC_COMPARE", "Failed to lock decoded BufferPool");
    }

    // 关键：callback 做 compare，但我们仍需“drain”释放 filled buffer，避免 decode worker 卡在 acquireFree
    int drained = 0;
    while (decoded_pool->isRunning() || decoded_pool->getFilledCount() > 0) {
        Buffer* buf = decoded_pool->acquireFilled(true, 200);
        if (!buf) {
            continue;
        }
        decoded_pool->releaseFilled(buf);
        drained++;
    }

    // 确保线程退出
    if (pipeline_started) {
        pipeline->stop();
        pipeline_started = false;
    }

    const int total = compare_ctx->total_frames.load(std::memory_order_relaxed);
    const auto end_time = std::chrono::steady_clock::now();
    result.duration_seconds = std::chrono::duration<double>(end_time - start_time).count();
    if (result.duration_seconds > 0.0) {
        result.average_fps = static_cast<double>(total) / result.duration_seconds;
    }

    ref_source.close();
    av_frame_free(&ref_avframe);

    result.frames_consumed = total;
    result.frames_compared = total;
    result.psnr_average = compare_ctx->getAveragePsnr();
    result.ssim_average = compare_ctx->getAverageSsim();
    result.compare_passed = compare_ctx->isPassed();
    result.success = result.compare_passed && total > 0;
    if (total <= 0) {
        result.error_message = "No frames compared - check encode/decode pipeline and reference file";
    }

    LOG4CPLUS_INFO_FMT(logger, "ENC_COMPARE done: compared=%d PSNR=%.2f SSIM=%.4f pass=%d drained=%d",
        total, result.psnr_average, result.ssim_average,
        result.compare_passed ? 1 : 0, drained);
    return result;
}

consumer::ConsumeResult runEncodeDecodeDisplay(
    const WorkerConfig& encode_cfg,
    const WorkerConfig& shared_cfg,
    const std::string& test_name)
{
    auto& logger = getLogger();
    consumer::ConsumeResult result;
    result.success = false;

    if (!test_name.empty()) {
        consumer::BufferConsumerService::printHeader(test_name, encode_cfg);
        LOG4CPLUS_INFO(logger, "");
        LOG4CPLUS_INFO(logger, "═══════════════════════════════════════════════════════");
        LOG4CPLUS_INFO_FMT(logger, "  Mode:       Encode -> decode -> Display (%s)", test_name.c_str());
        LOG4CPLUS_INFO(logger, "═══════════════════════════════════════════════════════");
    }

    WorkerConfig encode_decode_cfg = encode_cfg;
    encode_decode_cfg.consumer_type.compare.enable_psnr = false;
    encode_decode_cfg.consumer_type.compare.enable_ssim = false;

    MultiWorkerConfig multi_config;
    WorkerGroupConfig group("encode_decode_display_group");

    ProducerConfig encode_producer;
    encode_producer.producer_name = "encoder";
    encode_producer.worker_config = encode_decode_cfg;
    encode_producer.worker_config.worker_type = WorkerType::FFMPEG_ENCODE;
    group.producer_configs.push_back(encode_producer);

    ConsumerConfig decode_consumer;
    decode_consumer.consumer_name = "decoder";
    decode_consumer.worker_config = encode_decode_cfg;
    decode_consumer.worker_config.worker_type = WorkerType::FFMPEG_DECODE;
    decode_consumer.worker_config.data_source.buffer_mode = true;
    decode_consumer.worker_config.decoder.name = std::nullopt;
    // 为验证“硬件解码是否产不出帧”：display 路径改为软件解码。
    // 若软件解码有帧而硬件解码为 0 frames，则根因基本可定位到硬件解码的初始化/参数集缺失。
    decode_consumer.worker_config.decoder.enable_hardware = false;
    decode_consumer.worker_config.consumer_type.display.enable = false;
    decode_consumer.worker_config.consumer_type.save_raw = encode_decode_cfg.consumer_type.save_raw;
    decode_consumer.worker_config.consumer_type.save_encoded = encode_decode_cfg.consumer_type.save_encoded;
    group.consumer_configs.push_back(decode_consumer);

    ConnectorConfig decode_connector;
    decode_connector.mode = Connector::Mode::ONE_TO_ONE;
    decode_connector.producer_names = {"encoder"};
    decode_connector.consumer_names = {"decoder"};
    group.connector_configs.push_back(decode_connector);

    multi_config.groups.push_back(group);

    std::unique_ptr<MultiWorkerProductionLine> pipeline;
    bool pipeline_started = false;
    auto decoded_pool = std::shared_ptr<BufferPool>{};

    auto cleanup = [&]() {
        if (pipeline_started && pipeline) {
            pipeline->stop();
            pipeline_started = false;
        }
    };
    struct ScopeExit2 {
        std::function<void()> fn;
        ~ScopeExit2() { if (fn) fn(); }
    } scope_exit{cleanup};

    pipeline = std::make_unique<MultiWorkerProductionLine>(multi_config, encode_cfg.data_source.loop, 1, false);
    if (!pipeline->start()) {
        return failResult(logger, "ENC_DISPLAY", "Failed to start encode-decode pipeline");
    }
    pipeline_started = true;

    const uint64_t decoded_pool_id = pipeline->getGroupConsumerBufferPoolId(0, 0);
    if (decoded_pool_id == 0) {
        return failResult(logger, "ENC_DISPLAY", "Failed to get decoded BufferPool ID");
    }
    decoded_pool = BufferPoolRegistry::getInstance().getPool(decoded_pool_id).lock();
    if (!decoded_pool) {
        return failResult(logger, "ENC_DISPLAY", "Failed to lock decoded BufferPool");
    }

    uint32_t flags = test::ExecuteMode::buildConsumeFlags(shared_cfg);
    WorkerConfig consume_cfg = shared_cfg;
    // 使用 SHARED_FB：更符合 vdec 播放路径的默认假设
    consume_cfg.consumer_type.display.mode =
        WorkerConfig::ConsumerTypeConfig::DisplayType::DisplayMode::SHARED_FB;
    if (consume_cfg.consumer_type.max_timeout_count < 50) {
        consume_cfg.consumer_type.max_timeout_count = 50;
    }

    consumer::BufferConsumerService service;
    result = service.consumeExternalPool(decoded_pool, consume_cfg, flags);

    if (result.frames_consumed <= 0 && result.error_message.empty()) {
        result.success = false;
        result.error_message = "No frames displayed - check encode/decode pipeline";
        LOG4CPLUS_ERROR_FMT(logger, "[ENC_DISPLAY] %s", result.error_message.c_str());
    }

    return result;
}

} // namespace venc
} // namespace test
