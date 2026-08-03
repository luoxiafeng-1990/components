/**
 * @file VencPlugin.cpp
 * @brief 编码测试插件（预定义用例与备份分支 VencTestSuite::buildConfig 对齐）
 */

#include "VencPlugin.hpp"

#include "../common/third_party/CLI11.hpp"
#include "../common/WorkerConfigFactory.hpp"
#include "vendor/taco/encode/TacoEncoderExtension.hpp"

#include "consumptionline/core/BufferConsumerService.hpp"
#include "consumptionline/config/ConsumerTypeConfigBuilder.hpp"
#include "productionline/line/MultiWorkerProductionLine.hpp"
#include "productionline/line/WorkerSyncCoordinator.hpp"
#include "productionline/worker/config/FrameSyncTypes.hpp"
#include "productionline/worker/datasource/rawdata/RawFrameSourceFromFile.hpp"
#include "productionline/worker/base/ComponentTopology.hpp"
#include "bufferpool/buffer/Buffer.hpp"
#include "bufferpool/buffer/AVFrameBuffer.hpp"

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/imgutils.h>
#include <libavutil/frame.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

#include <cctype>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <algorithm>
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
    {"rgb444", "rgb444le"},
    {"bgr444", "bgr444le"},
    {"rgb555", "rgb555le"},
    {"bgr555", "bgr555le"},
    {"rgb565", "rgb565le"},
    {"bgr565", "bgr565le"},
    {"rgb888", "rgb888"},
    {"brg888", "brg888"},
    {"rgb101010", "rgb101010le"},
    {"brg101010", "brg101010"},
};

/** §2.2 JPEG：在 §2.1 基础上增加 BGR565 / RGB888 / BGR888 / 10bit RGB */
static const SpecFmt kSpec22JpegExtraFormats[] = {
    {"bgr565", "bgr565le"},
    {"rgb888", "rgb888"},
    {"bgr888", "bgr888"},
    {"rgb101010", "rgb101010le"},
    {"bgr101010", "bgr101010le"},
    {"brg888", "brg888"},
    {"brg101010", "brg101010"},
};

struct SpecRes {
    int w;
    int h;
    const char* tag;
};

static const SpecRes kSpec21Resolutions[] = {
    {144, 96, "144x96"},
    {320, 240, "320x240"},
    {512, 512, "512x512"},
    {640, 480, "640x480"},
    {1280, 720, "1280x720"},
    {1920, 1080, "1920x1080"},
};

static const SpecRes kSpec22JpegResolutions[] = {
    {96, 32, "96x32"},
    {144, 96, "144x96"},
    {320, 240, "320x240"},
    {640, 480, "640x480"},
    {512, 512, "512x512"},
    {1280, 720, "1280x720"},
    {1920, 1080, "1920x1080"},
    {2560, 1440, "2560x1440"},
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

static int mapPixFmtStringToAVPixFmt(std::string_view format_name) {
    std::string fmt(format_name);
    std::transform(fmt.begin(), fmt.end(), fmt.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (fmt == "nv12")        return AV_PIX_FMT_NV12;
    if (fmt == "nv21")        return AV_PIX_FMT_NV21;
    if (fmt == "yuv420p")     return AV_PIX_FMT_YUV420P;
    if (fmt == "yuvj420p")    return AV_PIX_FMT_YUVJ420P;
    if (fmt == "yuyv422" || fmt == "yuyv")   return AV_PIX_FMT_YUYV422;
    if (fmt == "yvyu")        return AV_PIX_FMT_YVYU422;
    if (fmt == "uyvy422" || fmt == "uyvy")   return AV_PIX_FMT_UYVY422;
    if (fmt == "rgb24"  || fmt == "rgb888")  return AV_PIX_FMT_RGB24;
    if (fmt == "bgr24"  || fmt == "bgr888")  return AV_PIX_FMT_BGR24;
    // TACO venc 无独立 BRG888：枚举侧按 BGR888 送编；资源仍用 *_brg888.rgb（勿偷换成 bgr 文件）
    if (fmt == "brg888" || fmt == "brg24")   return AV_PIX_FMT_BGR24;
    if (fmt == "argb"   || fmt == "argb888") return AV_PIX_FMT_ARGB;
    if (fmt == "bgra"   || fmt == "bgra888") return AV_PIX_FMT_BGRA;
    if (fmt == "rgba"   || fmt == "rgba888") return AV_PIX_FMT_RGBA;
    if (fmt == "abgr"   || fmt == "abgr888") return AV_PIX_FMT_ABGR;
    if (fmt == "rgb0"   || fmt == "rgbx888") return AV_PIX_FMT_RGB0;
    if (fmt == "bgr0"   || fmt == "bgrx888") return AV_PIX_FMT_BGR0;
    if (fmt == "rgb565" || fmt == "rgb565le") return AV_PIX_FMT_RGB565LE;
    if (fmt == "bgr565" || fmt == "bgr565le") return AV_PIX_FMT_BGR565LE;
    if (fmt == "rgb555" || fmt == "rgb555le") return AV_PIX_FMT_RGB555LE;
    if (fmt == "bgr555" || fmt == "bgr555le") return AV_PIX_FMT_BGR555LE;
    if (fmt == "rgb444" || fmt == "rgb444le") return AV_PIX_FMT_RGB444LE;
    if (fmt == "bgr444" || fmt == "bgr444le") return AV_PIX_FMT_BGR444LE;
    // 10bit packed：taco-ffmpeg（FFmpeg 4.2 fork）无 X2RGB10，约定用 RGB0/BGR0 作 32bpp
    // 容器送编（taco 侧 RGB0→RGB101010 / BGR0→BGR101010，字节原样进硬件）。
    // 对比侧不能按 8bit RGB0 理解这些字节，见 runEncodeQualityCompare 的 101010 unpack。
    if (fmt == "rgb101010" || fmt == "rgb101010le" || fmt == "rgbx101010"
        || fmt == "x2rgb10le")
        return AV_PIX_FMT_RGB0;
    if (fmt == "bgr101010" || fmt == "bgr101010le" || fmt == "bgrx101010"
        || fmt == "brg101010" || fmt == "brg101010le"
        || fmt == "x2bgr10le")
        return AV_PIX_FMT_BGR0;

    fprintf(stderr,
            "[ERROR] mapPixFmtStringToAVPixFmt: unrecognized format \"%s\" "
            "(refuse silent NV12 fallback)\n",
            std::string(format_name).c_str());
    return AV_PIX_FMT_NONE;
}

static bool isParallelProfile(const std::string& pr) {
    return pr.size() >= 10 && pr.compare(0, 9, "parallel_") == 0;
}

/** 判断 positional 是否像预定义测试名（而非文件路径）。
 *  未知预定义名必须报错，禁止当路径 / 静默忽略后走默认 1080p。 */
static bool looksLikePredefinedTestName(const std::string& arg) {
    if (arg.empty())
        return false;
    if (arg.find('/') != std::string::npos)
        return false;

    static const char* kExts[] = {
        ".yuv", ".rgb", ".raw", ".nv12", ".nv21",
        ".h264", ".h265", ".hevc", ".jpg", ".jpeg", ".mp4", ".mjpeg"
    };
    for (const char* e : kExts) {
        const size_t n = std::strlen(e);
        if (arg.size() >= n && arg.compare(arg.size() - n, n, e) == 0)
            return false;
    }

    const unsigned char c0 = static_cast<unsigned char>(arg[0]);
    if (!std::isalpha(c0) && arg[0] != '_')
        return false;
    for (char c : arg) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!(std::isalnum(uc) || c == '_'))
            return false;
    }
    return true;
}

static bool pathReadableFile(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

WorkerConfig buildEncodeConfigInternal(const EncodeTestParams& params,
                                       const std::string& yuv_path,
                                       int buffer_count = 16) {
    using test::common::WorkerConfigFactory;

    // ── 自动格式匹配 ──
    // 当预设指定的 input_format 与命令行 -i 文件名暗示的格式不一致时，
    // 自动在同目录和备用目录中搜索正确格式的输入文件。
    // 例：preset=rgb565le 但 -i /usr/data/ffmpeg/1920x1080_nv12.yuv
    //  → 自动替换为 /usr/data/ffmpeg/1920x1080_rgb565.rgb 或 /usr/data/qa/1920x1080_rgb565.rgb
    std::string resolved_path = yuv_path;
    {
        const auto& ifmt = params.input_format;

        // 非路径字符串（如未注册 profile 名）禁止偷换成默认 1920x1080_nv12.yuv
        const bool skip_auto_format =
            (yuv_path.find('/') == std::string::npos && !pathReadableFile(yuv_path));
        if (!skip_auto_format) {
            // 从格式名推断文件后缀中应该包含的关键词
            // nv12/nv21/yuv420p → _nv12.yuv / _nv21.yuv / _yuv420p.yuv
            // yuyv422/uyvy422 → _yuyv422.yuv / _uyvy422.yuv
            // rgb*/bgr*/brg* → _rgb*.rgb / _bgr*.rgb / _brg*.rgb
            auto fmtFileKey = [](const std::string& fmt) -> std::string {
                // 剥离尾部的 "le" / "be" 后缀（如 rgb565le → rgb565）
                std::string key = fmt;
                if (key.size() > 2 &&
                    (key.substr(key.size()-2) == "le" || key.substr(key.size()-2) == "be")) {
                    key = key.substr(0, key.size()-2);
                }
                return key;
            };

            std::string expected_key = fmtFileKey(ifmt);
            bool is_rgb_fmt = (expected_key.find("rgb") != std::string::npos ||
                               expected_key.find("bgr") != std::string::npos ||
                               expected_key.find("brg") != std::string::npos);

            // 检查当前文件名中是否包含预期的格式关键词
            bool file_matches = (yuv_path.find(expected_key) != std::string::npos);

            // 显式路径若已可读，且文件名未声明「其它」像素格式，则不要偷换成默认资源。
            // 否则 /tmp/foo.yuv、加长序列等会被替换成 /usr/data/ffmpeg/1920x1080_nv12.yuv（仅 10 帧），
            // 造成假测试 / 误归因。
            auto filenameMentionsOtherFormat = [&](const std::string& path) -> bool {
                static const char* kKnown[] = {
                    "nv12", "nv21", "yuv420p", "yuv420sp",
                    "yuyv422", "uyvy422", "rgb565", "rgb888", "bgr888",
                    "rgb101010", "bgr101010", "brg888", "brg101010"
                };
                for (const char* k : kKnown) {
                    if (expected_key.find(k) != std::string::npos) continue;
                    if (path.find(k) != std::string::npos) return true;
                }
                return false;
            };

            const bool keep_explicit_path =
                !file_matches && !yuv_path.empty() &&
                pathReadableFile(yuv_path) && !filenameMentionsOtherFormat(yuv_path);

            if (!file_matches && !yuv_path.empty() && !keep_explicit_path) {
                // 从原始路径中提取分辨率和目录
                std::string dir;
                std::string base;
                auto slash_pos = yuv_path.rfind('/');
                if (slash_pos != std::string::npos) {
                    dir = yuv_path.substr(0, slash_pos);
                    base = yuv_path.substr(slash_pos + 1);
                } else {
                    dir = ".";
                    base = yuv_path;
                }

                // 提取分辨率 WxH
                std::string res_tag;
                {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%dx%d", params.input_width, params.input_height);
                    res_tag = buf;
                }

                // 构建候选文件名
                std::string ext = is_rgb_fmt ? ".rgb" : ".yuv";
                std::string candidate_name = res_tag + "_" + expected_key + ext;

                // 在多个目录中搜索
                std::vector<std::string> search_dirs = {dir};
                // 添加备用目录
                if (dir.find("/usr/data/ffmpeg") != std::string::npos) {
                    search_dirs.push_back("/usr/data/qa");
                } else if (dir.find("/usr/data/qa") != std::string::npos) {
                    search_dirs.push_back("/usr/data/ffmpeg");
                } else {
                    search_dirs.push_back("/usr/data/ffmpeg");
                    search_dirs.push_back("/usr/data/qa");
                }

                for (const auto& sdir : search_dirs) {
                    std::string candidate = sdir + "/" + candidate_name;
                    FILE* f = fopen(candidate.c_str(), "r");
                    if (f) {
                        fclose(f);
                        fprintf(stderr,
                            "[venc] Auto-format resolve: %s → %s (input_format=%s)\n",
                            yuv_path.c_str(), candidate.c_str(), ifmt.c_str());
                        resolved_path = candidate;
                        break;
                    }
                }
            } else if (keep_explicit_path) {
                fprintf(stderr,
                    "[venc] Auto-format keep explicit path: %s (readable, no conflicting format tag; input_format=%s)\n",
                    yuv_path.c_str(), ifmt.c_str());
            }
        }
    }

    const int output_width = params.output_width > 0 ? params.output_width : params.input_width;
    const int output_height = params.output_height > 0 ? params.output_height : params.input_height;
    const double fps = params.output_fps > 0 ? params.output_fps : params.input_fps;
    const int gop = params.gop_size > 0 ? params.gop_size : 30;
    const int pix = mapPixFmtStringToAVPixFmt(params.input_format);
    if (pix == AV_PIX_FMT_NONE) {
        throw std::runtime_error(
            std::string("unsupported input_format: ") + params.input_format);
    }
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
            resolved_path, output_width, output_height, q, jfps, pix);
    } else if (is_hevc) {
        if (params.use_hardware) {
            int prof = 1;
            if (!isParallelProfile(params.profile) && params.profile == "main10")
                prof = 2;
            config = WorkerConfigFactory::createH265Encode(
                resolved_path, output_width, output_height, br_kbps, fps, gop, prof, pix);
            if (!isParallelProfile(params.profile)) {
                auto* ext = dynamic_cast<TacoEncoderExtension*>(config.encoder.vendor.get());
                if (ext) {
                    const int w = output_width;
                    const int h = output_height;
                    const double hfps = fps;
                    if (w <= 1280 && h <= 720 && hfps <= 30.0)
                        ext->level = 120;
                    else if (w <= 1920 && h <= 1080 && hfps <= 30.0)
                        ext->level = 150;
                    else if (w <= 1920 && h <= 1080 && hfps <= 60.0)
                        ext->level = 153;
                    else
                        ext->level = 150;
                }
            }
        } else {
            config = WorkerConfigFactory::createSoftwareEncode(
                resolved_path, "h265", output_width, output_height, br_kbps, fps, gop, pix);
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
                resolved_path, output_width, output_height, br_kbps, fps, gop, prof, pix);
            // 与 HEVC 分支一致：按分辨率/帧率设置 TACO level（供 FFmpeg 打开前经 options 下发）
            if (!isParallelProfile(params.profile)) {
                const int w = output_width;
                const int h = output_height;
                const double hfps = fps;
                int lvl = 150;
                if (w <= 1280 && h <= 720 && hfps <= 30.0)
                    lvl = 120;
                else if (w <= 1920 && h <= 1080 && hfps <= 30.0)
                    lvl = 150;
                else if (w <= 1920 && h <= 1080 && hfps <= 60.0)
                    lvl = 153;
                config.encoder.vendor = makeTacoEncoderExtension(0, lvl);
            }
        } else {
            config = WorkerConfigFactory::createSoftwareEncode(
                resolved_path, "h264", output_width, output_height, br_kbps, fps, gop, pix);
        }
    }

    if (isParallelProfile(params.profile)) {
        config.encoder.vendor = makeTacoEncoderExtension(0, 0);
    }

    config.encoder.rc_mode = params.rc_mode;
    config.encoder.cqp_qp = params.cqp_qp;
    // CQP 预定义常用 bitrate=0；TACO H264EncSetRateCtrl 在 bit_rate==0 时可能返回 -3，
    // 需提供名义码率供 VBV/内核填充（画质仍由 cqp_qp 决定）
    if (params.rc_mode == 2 && config.encoder.bit_rate <= 0) {
        const int fallback_kbps = params.bitrate > 0 ? params.bitrate : 4000;
        config.encoder.bit_rate = static_cast<int64_t>(fallback_kbps) * 1000;
    }
    // 大图缩放（如 spec24）：裸文件按 input 分辨率读，编码输出为 output（见 FFmpegEncodeWorker swscale）
    if (output_width > 0 && output_height > 0
        && (params.input_width != output_width || params.input_height != output_height)) {
        config.data_source.raw_frame_width = params.input_width;
        config.data_source.raw_frame_height = params.input_height;
    }
    config.data_source.buffer_count = buffer_count > 0 ? buffer_count : 16;
    config.data_source.buffer_mode = false;
    return config;
}

WorkerConfig VencPlugin::buildEncodeConfig(const EncodeTestParams& params) {
    const int bc = ds_opts_.buffer_count > 0 ? ds_opts_.buffer_count : kDefaultEncodeBufferCount;
    return buildEncodeConfigInternal(params, input_path_, bc);
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

    // ── 与备份 VencTestSuite 对齐的补充用例（码率/Profile/GOP/RC/并行等）──
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
    add("h264_2560x1440_30_6mbps", [&] {
        EncodeTestParams p("h264", "main", 6000, 30, true);
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
    addScale8192("jpeg", "96x32", 96, 32, 0);
    addScale8192("jpeg", "1280x720", 1280, 720, 0);
    addScale8192("jpeg", "1920x1080", 1920, 1080, 0);

    // ── 非对称边界分辨率（飞书 Procedure 使用的 profile 名）──
    auto addBoundaryH264Nv12 = [&](const char* name, int w, int h) {
        EncodeTestParams p("h264", "main", matrixBitrateKbps(w, h), 30, true);
        p.input_format = "nv12";
        p.input_width = w;
        p.input_height = h;
        p.input_fps = 30.0;
        add(name, std::move(p));
    };
    addBoundaryH264Nv12("boundary_h264_nv12_144x1080", 144, 1080);
    addBoundaryH264Nv12("boundary_h264_nv12_1920x96", 1920, 96);

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

    // ── 飞书测试用例补全：GOP 变体 ──
    add("h264_1920x1080_30_4mbps_gop300", [&] {
        EncodeTestParams p("h264", "main", 4000, 300, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 30.0;
        return p;
    }());
    add("h264_1920x1080_30_4mbps_gop600", [&] {
        EncodeTestParams p("h264", "main", 4000, 600, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 30.0;
        return p;
    }());

    // ── VBR 变体 ──
    add("h264_640x480_30_2mbps_vbr", [&] {
        EncodeTestParams p("h264", "main", 2000, 30, true);
        p.input_format = "nv12";
        p.input_width = 640;
        p.input_height = 480;
        p.input_fps = 30.0;
        p.rc_mode = 1;
        return p;
    }());
    add("h264_1280x720_30_4mbps_vbr", [&] {
        EncodeTestParams p("h264", "main", 4000, 30, true);
        p.input_format = "nv12";
        p.input_width = 1280;
        p.input_height = 720;
        p.input_fps = 30.0;
        p.rc_mode = 1;
        return p;
    }());

    // ── 非标准分辨率（鲁棒性测试）──
    add("h264_1921x1081_30_4mbps", [&] {
        EncodeTestParams p("h264", "main", 4000, 30, true);
        p.input_format = "nv12";
        p.input_width = 1921;
        p.input_height = 1081;
        p.input_fps = 30.0;
        return p;
    }());
    add("h264_1279x719_30_3mbps", [&] {
        EncodeTestParams p("h264", "main", 3000, 30, true);
        p.input_format = "nv12";
        p.input_width = 1279;
        p.input_height = 719;
        p.input_fps = 30.0;
        return p;
    }());

    // ── JPEG 质量变体 ──
    add("jpeg_1920x1080_q1", [&] {
        EncodeTestParams p("jpeg", "", 0, 0, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 25.0;
        p.jpeg_quality = 1;
        return p;
    }());
    add("jpeg_1920x1080_q50", [&] {
        EncodeTestParams p("jpeg", "", 0, 0, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 25.0;
        p.jpeg_quality = 50;
        return p;
    }());
    add("jpeg_1920x1080_q95", [&] {
        EncodeTestParams p("jpeg", "", 0, 0, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 25.0;
        p.jpeg_quality = 95;
        return p;
    }());
    add("jpeg_1920x1080_q100", [&] {
        EncodeTestParams p("jpeg", "", 0, 0, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 25.0;
        p.jpeg_quality = 100;
        return p;
    }());
    add("jpeg_2560x1440_q80", [&] {
        EncodeTestParams p("jpeg", "", 0, 0, true);
        p.input_format = "nv12";
        p.input_width = 2560;
        p.input_height = 1440;
        p.input_fps = 25.0;
        p.jpeg_quality = 80;
        return p;
    }());

    // ── 鲁棒性：极端参数组合 ──
    add("robust_h264_trm_min_144x96_high_br", [&] {
        EncodeTestParams p("h264", "main", 50000, 30, true);
        p.input_format = "nv12";
        p.input_width = 144;
        p.input_height = 96;
        p.input_fps = 30.0;
        return p;
    }());
    add("robust_jpeg_trm_min_96x32_q1", [&] {
        EncodeTestParams p("jpeg", "", 0, 0, true);
        p.input_format = "nv12";
        p.input_width = 96;
        p.input_height = 32;
        p.input_fps = 25.0;
        p.jpeg_quality = 1;
        return p;
    }());

    // ── §2.7 补充：nv21 ──
    add("spec27_h264_1080p_25fps_nv21", [&] {
        EncodeTestParams p("h264", "main", 5000, 30, true);
        p.input_format = "nv21";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 25.0;
        return p;
    }());

    // ── 硬件格式校验（hwfmt_chk）：H.264 × 所有像素格式 ──
    {
        struct HwfmtEntry { const char* key; const char* pix; };
        static const HwfmtEntry kHwfmtAll[] = {
            {"yuv420p","yuv420p"}, {"nv12","nv12"}, {"nv21","nv21"},
            {"yuyv422","yuyv422"}, {"uyvy422","uyvy422"},
            {"rgb444","rgb444le"}, {"bgr444","bgr444le"},
            {"rgb555","rgb555le"}, {"bgr555","bgr555le"},
            {"rgb565","rgb565le"}, {"bgr565","bgr565le"},
            {"rgb888","rgb888"}, {"bgr888","bgr888"}, {"brg888","brg888"},
            {"rgb101010","rgb101010le"}, {"bgr101010","bgr101010le"},
            {"brg101010","brg101010"},
            {"yuv420sp","nv12"}, {"yuv420sp_vu","nv21"},
        };
        for (const auto& e : kHwfmtAll) {
            // H.264 hwfmt_chk
            {
                std::string name = std::string("h264_hwfmt_chk_") + e.key + "_1920x1080";
                EncodeTestParams p("h264", "main", 6000, 30, true);
                p.input_format = e.pix;
                p.input_width = 1920;
                p.input_height = 1080;
                p.input_fps = 30.0;
                add(name.c_str(), std::move(p));
            }
            // JPEG hwfmt_chk (skip yuv420sp aliases for jpeg)
            if (std::string(e.key).find("yuv420sp") == std::string::npos) {
                std::string name = std::string("jpeg_hwfmt_chk_") + e.key + "_1920x1080";
                EncodeTestParams p("jpeg", "", 0, 0, true);
                p.input_format = e.pix;
                p.input_width = 1920;
                p.input_height = 1080;
                p.input_fps = 25.0;
                p.jpeg_quality = 80;
                add(name.c_str(), std::move(p));
            }
        }
    }

    // ── 鲁棒性补充：极端码率/CQP/Profile/组合 ──
    add("robust_h264_1080p_low_bitrate_500k", [&] {
        EncodeTestParams p("h264", "main", 500, 30, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 30.0;
        p.rc_mode = 1;
        return p;
    }());
    add("robust_h264_1080p_high_bitrate_20mbps", [&] {
        EncodeTestParams p("h264", "main", 20000, 30, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 30.0;
        return p;
    }());
    add("robust_h264_1080p_gop600_vbr", [&] {
        EncodeTestParams p("h264", "main", 4000, 600, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 30.0;
        p.rc_mode = 1;
        return p;
    }());
    add("robust_h264_1080p_cqp_qp1", [&] {
        EncodeTestParams p("h264", "main", 0, 30, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 30.0;
        p.rc_mode = 2;
        p.cqp_qp = 1;
        return p;
    }());
    add("robust_h264_1080p_cqp_qp10", [&] {
        EncodeTestParams p("h264", "main", 0, 30, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 30.0;
        p.rc_mode = 2;
        p.cqp_qp = 10;
        return p;
    }());
    add("robust_h264_1080p_cqp_qp51", [&] {
        EncodeTestParams p("h264", "main", 0, 30, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 30.0;
        p.rc_mode = 2;
        p.cqp_qp = 51;
        return p;
    }());
    add("robust_h264_baseline_720p_4mbps", [&] {
        EncodeTestParams p("h264", "baseline", 4000, 30, true);
        p.input_format = "nv12";
        p.input_width = 1280;
        p.input_height = 720;
        p.input_fps = 30.0;
        return p;
    }());
    add("robust_h264_1080p_gop1_cbr_1mbps", [&] {
        EncodeTestParams p("h264", "main", 1000, 1, true);
        p.input_format = "nv12";
        p.input_width = 1920;
        p.input_height = 1080;
        p.input_fps = 30.0;
        p.rc_mode = 0;
        return p;
    }());
    add("robust_jpeg_8192_nv12_q80", [&] {
        EncodeTestParams p("jpeg", "", 0, 0, true);
        p.input_format = "nv12";
        p.input_width = 8192;
        p.input_height = 8192;
        p.input_fps = 25.0;
        p.jpeg_quality = 80;
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
    app.add_option("-c,--codec", params_.codec, "编码格式 h264|jpeg");
    app.add_option("-P,--profile", params_.profile, "Profile (baseline|main|high|main10)");
    app.add_option("-b,--bitrate", params_.bitrate, "码率 (kbps)");
    app.add_option("--cqp-qp", params_.cqp_qp, "CQP 模式量化参数 1–51（rc_mode=2 / 预定义 CQP 用例）");
    app.add_option("-g,--gop", params_.gop_size, "GOP 大小");
    app.add_option("-W,--width", params_.input_width, "输入宽度");
    app.add_option("-H,--height", params_.input_height, "输入高度");
    app.add_option("-F,--fps", params_.input_fps, "输入/编码帧率");
    app.add_option("--output-fps", params_.output_fps, "输出帧率（默认跟随输入帧率）");
    app.add_option("-f,--input-format", params_.input_format, "输入像素格式 (默认 nv12)");
    app.add_option("-m,--max-frames", max_frames_, "最大帧数 (-1=不限制)");
    app.add_option("--loop", loop_count_,
        "输入文件循环遍数（默认 1；有效总帧数=文件帧数×遍数）");
    app.add_option("-o,--output", encoded_output_path_,
        "编码输出路径：推荐 .mp4 封装便于通用播放器/ vdec 回放；亦支持 .h264/.265/.mjpeg 等（扩展名决定复用器）");
    app.add_flag("-v,--verbose", verbose_, "详细日志");
    app.add_option("-t,--threads", threads_, "并行通道数 (PARALLEL 模式)");

    // COMPARE 横切选项（阈值短选项保持 venc/stress 历史：-M/-N）
    compare_opts_.registerTo(app, CompareOptions::ThresholdStyle::Venc, 30.0, 0.95);

    // venc 的 -b 已被 bitrate 占用，仅注册长选项 --buffer-count
    ds_opts_.registerTo(app, /*with_short_b=*/false);

    app.add_option("positional", positional_args_, "预定义测试名或输入文件");

    app.footer(
        "示例:\n"
        "  qa_cases venc -l\n"
        "  qa_cases venc h264_1920x1080_60_8mbps /data/in.nv12\n"
        "  qa_cases venc -p -S -M 38 -N 0.95 h264_1920x1080_60_8mbps /data/in.nv12\n"
        "  # COMPARE 默认 --compare-target source-ref（源↔编码→软解）\n"
        "  qa_cases venc --loop 3 -p -S h264_1920x1080_30_8mbps /data/in.nv12\n"
        "  qa_cases venc --buffer-count 16 -p -S h264_1920x1080_30_8mbps /data/in.nv12\n"
        "  qa_cases venc -t 4 -o /tmp/o.mp4 h264_1920x1080_30_4mbps /data/in.nv12\n"
        "  qa_cases venc h264_1920x1080_30_4mbps /data/in.nv12 display\n"
        "  qa_cases venc h264_1920x1080_30_4mbps /data/in.nv12 display --vendor taco --fps 30\n");
}

void VencPlugin::applyCliToConfig(WorkerConfig& config) const {
    if (!input_path_.empty())
        config.data_source.path = input_path_;
    config.data_source.max_frames = max_frames_;
    config.data_source.loop_count = loop_count_ < 1 ? 1 : loop_count_;
    // CLI --buffer-count；未指定时保持 0，由 buildEncodeConfigInternal 落成默认 16
    ds_opts_.applyCliToConfig(config);

    auto ct_builder = ConsumerTypeConfigBuilder(config.consumer_type)
        .setVerbose(verbose_);

    if (!encoded_output_path_.empty()) {
        ct_builder.setSaveEncodedConfig(SaveEncodedConfigBuilder()
            .setEnable(true)
            .setOutputPath(encoded_output_path_)
            .build());
    }

    config.consumer_type = ct_builder.build();

    // COMPARE 默认 source-ref（源↔编码→软解）；未指定 producer 时补 FFMPEG_ENCODE
    compare_opts_.applyCliToConfig(config, ConsumerTypeConfig::CompareType::TARGET_SOURCE_REF);
    if (compare_opts_.isCompareEnabled() && config.mg_datasource_producer_type.empty()) {
        config.mg_datasource_producer_type = "FFMPEG_ENCODE";
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
        // 未知预定义名：必须失败退出。旧逻辑会把名字当路径或静默忽略，
        // 再被 Auto-format 偷换成 1920x1080_nv12.yuv，造成假 PASS。
        if (looksLikePredefinedTestName(arg)) {
            std::cerr << "Error: 未知预定义测试名 \"" << arg
                      << "\"（不在 qa_cases venc -l 列表中）。\n"
                      << "禁止将未知 profile 当作输入路径或静默忽略。\n"
                      << "请使用已注册名称，或显式 -i <文件> 并配合 -c/-W/-H/-f 参数。\n";
            return 1;
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
        const int bc = shared_config.data_source.buffer_count > 0
            ? shared_config.data_source.buffer_count
            : (ds_opts_.buffer_count > 0 ? ds_opts_.buffer_count : kDefaultEncodeBufferCount);
        for (int i = 0; i < thread_count; i++) {
            WorkerConfig cfg = buildEncodeConfigInternal(p, shared_config.data_source.path, bc);
            cfg.data_source.max_frames = base.data_source.max_frames;
            cfg.data_source.loop = base.data_source.loop;
            cfg.data_source.loop_count = base.data_source.loop_count;

            auto ct_builder = ConsumerTypeConfigBuilder(base.consumer_type)
                .setPerformanceConfig(PerformanceConfigBuilder(base.consumer_type.performance)
                    .setTargetFps(p.input_fps)
                    .build());
            if (!out_paths.empty()) {
                ct_builder.setSaveEncodedConfig(SaveEncodedConfigBuilder()
                    .setEnable(true)
                    .setOutputPath(
                        (static_cast<size_t>(i) < out_paths.size()) ? out_paths[static_cast<size_t>(i)] : out_paths[0])
                    .build());
            }
            cfg.consumer_type = ct_builder.build();
            cfg.mg_datasource_producer_type = shared_config.mg_datasource_producer_type;
            configs.push_back(std::move(cfg));
        }
        return configs;
    }

    const int bc = shared_config.data_source.buffer_count > 0
        ? shared_config.data_source.buffer_count
        : (ds_opts_.buffer_count > 0 ? ds_opts_.buffer_count : kDefaultEncodeBufferCount);
    WorkerConfig full = buildEncodeConfigInternal(p, shared_config.data_source.path, bc);
    full.consumer_type = ConsumerTypeConfigBuilder(shared_config.consumer_type)
        .setPerformanceConfig(PerformanceConfigBuilder(shared_config.consumer_type.performance)
            .setTargetFps(p.input_fps)
            .build())
        .build();
    full.data_source.max_frames = shared_config.data_source.max_frames;
    full.data_source.loop = shared_config.data_source.loop;
    full.data_source.loop_count = shared_config.data_source.loop_count;
    full.mg_datasource_producer_type = shared_config.mg_datasource_producer_type;
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

/** 将 packed 10:10:10（X2RGB10 / X2BGR10 LE）解到 RGB24。10bit→8bit 用 >>2。 */
static void unpackPacked101010ToRgb24(
    const AVFrame* src, AVFrame* dst, bool is_bgr_order)
{
    const int w = src->width;
    const int h = src->height;
    for (int y = 0; y < h; ++y) {
        const uint8_t* srow = src->data[0] + y * src->linesize[0];
        uint8_t* drow = dst->data[0] + y * dst->linesize[0];
        for (int x = 0; x < w; ++x) {
            uint32_t v;
            memcpy(&v, srow + x * 4, 4);
            // X2RGB10LE: (msb)2X 10R 10G 10B(lsb)
            // X2BGR10LE: (msb)2X 10B 10G 10R(lsb)
            const int c0 = static_cast<int>(v & 0x3FFu);
            const int c1 = static_cast<int>((v >> 10) & 0x3FFu);
            const int c2 = static_cast<int>((v >> 20) & 0x3FFu);
            uint8_t r, g, b;
            if (is_bgr_order) {
                r = static_cast<uint8_t>(c0 >> 2);
                g = static_cast<uint8_t>(c1 >> 2);
                b = static_cast<uint8_t>(c2 >> 2);
            } else {
                b = static_cast<uint8_t>(c0 >> 2);
                g = static_cast<uint8_t>(c1 >> 2);
                r = static_cast<uint8_t>(c2 >> 2);
            }
            drow[x * 3 + 0] = r;
            drow[x * 3 + 1] = g;
            drow[x * 3 + 2] = b;
        }
    }
}

static bool pathLooksLikePacked101010(const std::string& path, bool* is_bgr) {
    const bool is_rgb = path.find("rgb101010") != std::string::npos;
    const bool is_bgr101010 = path.find("bgr101010") != std::string::npos;
    const bool is_brg101010 = path.find("brg101010") != std::string::npos;
    if (!is_rgb && !is_bgr101010 && !is_brg101010)
        return false;
    if (is_bgr)
        *is_bgr = is_bgr101010 || is_brg101010;
    return true;
}

/** 子字节 packed RGB：不能把解码 YUV 再压回 444/555/565 后对比（二次量化会假 FAIL）。 */
static bool isSubBytePackedRgb(AVPixelFormat fmt) {
    switch (fmt) {
        case AV_PIX_FMT_RGB444LE:
        case AV_PIX_FMT_RGB444BE:
        case AV_PIX_FMT_BGR444LE:
        case AV_PIX_FMT_BGR444BE:
        case AV_PIX_FMT_RGB555LE:
        case AV_PIX_FMT_RGB555BE:
        case AV_PIX_FMT_BGR555LE:
        case AV_PIX_FMT_BGR555BE:
        case AV_PIX_FMT_RGB565LE:
        case AV_PIX_FMT_RGB565BE:
        case AV_PIX_FMT_BGR565LE:
        case AV_PIX_FMT_BGR565BE:
            return true;
        default:
            return false;
    }
}

struct EncodeQualityCompareCallbackCtx {
    std::shared_ptr<CompareCallbackContext> compare_ctx;
    RawFrameSourceFromFile* ref_source = nullptr;
    AVFrame* ref_avframe = nullptr;       // 对比用（已对齐编码输出尺寸）
    AVFrame* ref_file_avframe = nullptr;  // 从文件读取的原始尺寸
    SwsContext* ref_scale_sws = nullptr;  // 文件尺寸 → 输出尺寸
    Buffer* ref_buf_wrap = nullptr;

    // 解码帧格式转换（解码输出 YUV → 输入格式 RGB / RGB24）
    AVPixelFormat input_pix_fmt = AV_PIX_FMT_NONE;
    SwsContext* decode_sws_ctx = nullptr;
    AVFrame* decode_converted_frame = nullptr;

    // rgb/bgr/brg101010：参考帧按 X2RGB10/X2BGR10 解包到 RGB24 再比（不改 taco-ffmpeg）
    bool packed_101010 = false;
    bool packed_101010_is_bgr = false;  // bgr101010 / brg101010
    // 444/555/565：参考帧 swscale → YUV420P，解码 YUV 直接比（避免 YUV→packed 二次量化）
    bool subbyte_rgb_to_rgb24 = false;
    SwsContext* ref_sws_ctx = nullptr;
    AVFrame* ref_rgb24_frame = nullptr;

    // 递增帧计数器（当 PTS 无效时用作参考帧索引）
    std::atomic<int64_t> frame_counter{0};

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

    const int out_width = encode_cfg.encoder.width;
    const int out_height = encode_cfg.encoder.height;
    // 缩放用例（spec24）：文件按 raw_frame_* 读，对比目标为编码输出尺寸
    const int file_width = (encode_cfg.data_source.raw_frame_width > 0)
        ? encode_cfg.data_source.raw_frame_width : out_width;
    const int file_height = (encode_cfg.data_source.raw_frame_height > 0)
        ? encode_cfg.data_source.raw_frame_height : out_height;
    const bool ref_needs_scale =
        (file_width != out_width || file_height != out_height);
    const int ref_width = out_width;
    const int ref_height = out_height;
    const AVPixelFormat ref_pix_fmt =
        static_cast<AVPixelFormat>(encode_cfg.encoder.input_pix_fmt);

    // 1) 准备参考帧数据源（按文件真实分辨率打开，避免把 8192 文件当 1280 读）
    RawFrameSourceFromFile ref_source(
        encode_cfg.data_source.path, file_width, file_height, ref_pix_fmt);
    if (!ref_source.open()) {
        return failResult(
            logger, "ENC_COMPARE", "Failed to open reference input file: " + encode_cfg.data_source.path);
    }

    AVFrame* ref_file_avframe = av_frame_alloc();
    AVFrame* ref_avframe = av_frame_alloc();
    if (!ref_file_avframe || !ref_avframe) {
        if (ref_file_avframe) av_frame_free(&ref_file_avframe);
        if (ref_avframe) av_frame_free(&ref_avframe);
        ref_source.close();
        return failResult(logger, "ENC_COMPARE", "Failed to allocate reference AVFrame");
    }
    ref_file_avframe->format = ref_pix_fmt;
    ref_file_avframe->width = file_width;
    ref_file_avframe->height = file_height;
    if (av_frame_get_buffer(ref_file_avframe, 64) < 0) {
        av_frame_free(&ref_file_avframe);
        av_frame_free(&ref_avframe);
        ref_source.close();
        return failResult(logger, "ENC_COMPARE", "Failed to allocate buffer for reference AVFrame");
    }
    ref_avframe->format = ref_pix_fmt;
    ref_avframe->width = ref_width;
    ref_avframe->height = ref_height;
    if (av_frame_get_buffer(ref_avframe, 64) < 0) {
        av_frame_free(&ref_file_avframe);
        av_frame_free(&ref_avframe);
        ref_source.close();
        return failResult(logger, "ENC_COMPARE", "Failed to allocate scaled reference AVFrame");
    }

    SwsContext* ref_scale_sws = nullptr;
    if (ref_needs_scale) {
        ref_scale_sws = sws_getContext(
            file_width, file_height, ref_pix_fmt,
            ref_width, ref_height, ref_pix_fmt,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!ref_scale_sws) {
            av_frame_free(&ref_file_avframe);
            av_frame_free(&ref_avframe);
            ref_source.close();
            return failResult(logger, "ENC_COMPARE",
                "Failed to create ref scale sws (file→encode output)");
        }
        LOG4CPLUS_INFO_FMT(logger,
            "  Ref scale for compare: %dx%d → %dx%d (match encode output)",
            file_width, file_height, ref_width, ref_height);
    }

    const int ref_buf_size = static_cast<int>(
        av_image_get_buffer_size(ref_pix_fmt, ref_width, ref_height, 1));

    // 先用 frame->data[0] 构造 Buffer wrapper；具体元数据在 callback 内逐帧刷新
    AVFrameBuffer ref_buf_wrap(
        0, ref_avframe->data[0], 0, static_cast<size_t>(ref_buf_size),
        Buffer::Ownership::EXTERNAL);
    ref_buf_wrap.setAVFrame(ref_avframe);

    // 2) compare_ctx：沿用 WorkerSyncCoordinator 的 BufferComparator 累积统计
    auto compare_ctx = std::make_shared<CompareCallbackContext>();
    compare_ctx->initFromCompareType(shared_cfg.consumer_type.compare);
    LOG4CPLUS_INFO_FMT(logger,
        "  Compare thresholds: min_psnr=%.2f min_ssim=%.4f enable_psnr=%d enable_ssim=%d",
        compare_ctx->min_psnr, compare_ctx->min_ssim,
        (int)compare_ctx->enable_psnr, (int)compare_ctx->enable_ssim);
    if (!compare_ctx->openComparator()) {
        if (ref_scale_sws) sws_freeContext(ref_scale_sws);
        av_frame_free(&ref_file_avframe);
        av_frame_free(&ref_avframe);
        ref_source.close();
        return failResult(logger, "ENC_COMPARE", "Failed to open BufferComparator");
    }

    // 3) 准备解码帧格式转换：当输入不是 YUV420P/NV12 时，
    //    解码器输出 YUV420P 需要转换回输入格式再对比（同格式对比）
    //    特例：
    //    - *101010：资源是真 10bit packed，送编用 RGB0/BGR0；双方转到 RGB24
    //    - *444/*555/*565：禁止 YUV→packed 再比（二次量化假 FAIL）；
    //      参考帧转到 YUV420P，与解码输出直接在 YUV 空间比（业界常用）
    AVFrame* decode_converted_frame = nullptr;
    SwsContext* decode_sws_ctx = nullptr;
    SwsContext* ref_sws_ctx = nullptr;
    AVFrame* ref_rgb24_frame = nullptr;  // 101010 unpack 目标；也复用为 subbyte→YUV 目标帧
    bool packed_101010 = false;
    bool packed_101010_is_bgr = false;
    packed_101010 = pathLooksLikePacked101010(encode_cfg.data_source.path, &packed_101010_is_bgr);
    const bool subbyte_rgb_to_yuv = isSubBytePackedRgb(ref_pix_fmt);

    AVPixelFormat compare_pix_fmt = ref_pix_fmt;
    if (packed_101010)
        compare_pix_fmt = AV_PIX_FMT_RGB24;
    else if (subbyte_rgb_to_yuv)
        compare_pix_fmt = AV_PIX_FMT_YUV420P;

    const bool need_decode_fmt_conv = (compare_pix_fmt != AV_PIX_FMT_YUV420P &&
                                       compare_pix_fmt != AV_PIX_FMT_NV12);
    if (need_decode_fmt_conv) {
        decode_converted_frame = av_frame_alloc();
        if (decode_converted_frame) {
            decode_converted_frame->format = compare_pix_fmt;
            decode_converted_frame->width = ref_width;
            decode_converted_frame->height = ref_height;
            if (av_frame_get_buffer(decode_converted_frame, 64) < 0) {
                av_frame_free(&decode_converted_frame);
                decode_converted_frame = nullptr;
            }
        }
        if (decode_converted_frame) {
            decode_sws_ctx = sws_getContext(
                ref_width, ref_height, AV_PIX_FMT_YUV420P,
                ref_width, ref_height, compare_pix_fmt,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (decode_sws_ctx) {
                LOG4CPLUS_INFO_FMT(logger,
                    "  Decode fmt conv: YUV420P → %s (same-format comparison)%s",
                    av_get_pix_fmt_name(compare_pix_fmt),
                    packed_101010 ? " [101010→RGB24]" : "");
            }
        }
    }

    if (packed_101010) {
        ref_rgb24_frame = av_frame_alloc();
        if (ref_rgb24_frame) {
            ref_rgb24_frame->format = AV_PIX_FMT_RGB24;
            ref_rgb24_frame->width = ref_width;
            ref_rgb24_frame->height = ref_height;
            if (av_frame_get_buffer(ref_rgb24_frame, 64) < 0) {
                av_frame_free(&ref_rgb24_frame);
                ref_rgb24_frame = nullptr;
            }
        }
        if (!ref_rgb24_frame || !decode_sws_ctx || !decode_converted_frame) {
            if (decode_sws_ctx) sws_freeContext(decode_sws_ctx);
            if (decode_converted_frame) av_frame_free(&decode_converted_frame);
            if (ref_rgb24_frame) av_frame_free(&ref_rgb24_frame);
            if (ref_scale_sws) sws_freeContext(ref_scale_sws);
            av_frame_free(&ref_file_avframe);
            av_frame_free(&ref_avframe);
            ref_source.close();
            return failResult(logger, "ENC_COMPARE",
                "Failed to prepare 101010→RGB24 compare buffers");
        }
        LOG4CPLUS_INFO_FMT(logger,
            "  Ref 101010 unpack: %s packed10 → RGB24 (is_bgr=%d)",
            encode_cfg.data_source.path.c_str(), (int)packed_101010_is_bgr);
    } else if (subbyte_rgb_to_yuv) {
        // 复用 ref_rgb24_frame 指针承载 YUV420P 参考帧
        ref_rgb24_frame = av_frame_alloc();
        if (ref_rgb24_frame) {
            ref_rgb24_frame->format = AV_PIX_FMT_YUV420P;
            ref_rgb24_frame->width = ref_width;
            ref_rgb24_frame->height = ref_height;
            if (av_frame_get_buffer(ref_rgb24_frame, 64) < 0) {
                av_frame_free(&ref_rgb24_frame);
                ref_rgb24_frame = nullptr;
            }
        }
        if (ref_rgb24_frame) {
            ref_sws_ctx = sws_getContext(
                ref_width, ref_height, ref_pix_fmt,
                ref_width, ref_height, AV_PIX_FMT_YUV420P,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
        }
        if (!ref_rgb24_frame || !ref_sws_ctx) {
            if (ref_sws_ctx) sws_freeContext(ref_sws_ctx);
            if (ref_rgb24_frame) av_frame_free(&ref_rgb24_frame);
            if (ref_scale_sws) sws_freeContext(ref_scale_sws);
            av_frame_free(&ref_file_avframe);
            av_frame_free(&ref_avframe);
            ref_source.close();
            return failResult(logger, "ENC_COMPARE",
                "Failed to prepare subbyteRGB→YUV420P compare buffers");
        }
        LOG4CPLUS_INFO_FMT(logger,
            "  Ref subbyte RGB: %s → YUV420P (avoid YUV→packed requantize)",
            av_get_pix_fmt_name(ref_pix_fmt));
    }

    EncodeQualityCompareCallbackCtx cb_ctx;
    cb_ctx.compare_ctx = compare_ctx;
    cb_ctx.ref_source = &ref_source;
    cb_ctx.ref_avframe = ref_avframe;
    cb_ctx.ref_file_avframe = ref_file_avframe;
    cb_ctx.ref_scale_sws = ref_scale_sws;
    cb_ctx.ref_buf_wrap = &ref_buf_wrap;
    cb_ctx.input_pix_fmt = ref_pix_fmt;
    cb_ctx.decode_sws_ctx = decode_sws_ctx;
    cb_ctx.decode_converted_frame = decode_converted_frame;
    cb_ctx.packed_101010 = packed_101010;
    cb_ctx.packed_101010_is_bgr = packed_101010_is_bgr;
    cb_ctx.subbyte_rgb_to_rgb24 = subbyte_rgb_to_yuv;  // 字段名历史兼容：现表示转中间格式再比
    cb_ctx.ref_sws_ctx = ref_sws_ctx;
    cb_ctx.ref_rgb24_frame = ref_rgb24_frame;

    // 4) MultiWorker：encoder -> decoder，并用 callback 做 compare
    MultiWorkerConfig multi_config;
    GroupConfig group("encode_decode_quality_compare_group");

    {
        WorkerConfig enc_wc = encode_cfg;
        enc_wc.global.worker_type = WorkerType::FFMPEG_ENCODE;
        enc_wc.consumer_type = ConsumerTypeConfigBuilder(enc_wc.consumer_type)
            .setDisplayConfig(DisplayConsumerConfigBuilder(enc_wc.consumer_type.display)
                .setEnable(false)
                .build())
            .build();
        group.producers["encoder"] = enc_wc;
    }

    {
        WorkerConfig dec_wc = encode_cfg;
        dec_wc.global.worker_type = WorkerType::FFMPEG_DECODE;
        dec_wc.data_source.buffer_mode = true;
        dec_wc.decoder.name = std::nullopt;
        dec_wc.decoder.enable_hardware = false;
        dec_wc.consumer_type = ConsumerTypeConfigBuilder(dec_wc.consumer_type)
            .setDisplayConfig(DisplayConsumerConfigBuilder(dec_wc.consumer_type.display)
                .setEnable(false)
                .build())
            .setCompareConfig(CompareConfigBuilder(dec_wc.consumer_type.compare)
                .setEnablePsnr(false)
                .setEnableSsim(false)
                .build())
            .build();
        group.consumers["decoder"] = dec_wc;
    }

    group.mode = GroupConfig::Mode::ONE_TO_ONE;
    group.enable_frame_sync = true;

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
        
        int64_t target_pts = decoded_buf->getPts();
        
        // 编码器循环读取源帧，解码器 PTS 可能无效（JPEG: AV_NOPTS_VALUE）
        // 或持续递增超出参考帧范围。使用帧计数器 + 取模对齐参考帧索引。
        const int total_ref = cb->ref_source->getTotalFrames();
        int64_t ref_frame_idx = cb->frame_counter.fetch_add(1, std::memory_order_relaxed);
        if (total_ref > 0) {
            ref_frame_idx = ref_frame_idx % total_ref;
        }
        int ref_ret = cb->ref_source->readRawFrameByPts(
            ref_frame_idx,
            cb->ref_file_avframe ? cb->ref_file_avframe : cb->ref_avframe);
        
        if (ref_ret == AVERROR_EOF || ref_ret < 0) {
            cb->ref_eof.store(true, std::memory_order_release);
            return true;
        }

        if (cb->ref_scale_sws && cb->ref_file_avframe && cb->ref_avframe) {
            sws_scale(cb->ref_scale_sws,
                cb->ref_file_avframe->data, cb->ref_file_avframe->linesize,
                0, cb->ref_file_avframe->height,
                cb->ref_avframe->data, cb->ref_avframe->linesize);
            cb->ref_avframe->pts = cb->ref_file_avframe->pts;
            cb->ref_buf_wrap->setAVFrame(cb->ref_avframe);
        } else {
            cb->ref_buf_wrap->setAVFrame(
                cb->ref_file_avframe ? cb->ref_file_avframe : cb->ref_avframe);
        }

        // ── 解码帧格式转换：将解码器输出(YUV420P)转回输入格式 ──
        // 确保同格式对比：RGB565 vs RGB565，而不是 RGB565 vs YUV420P
        // 101010：参考帧 unpack→RGB24，解码帧 YUV→RGB24
        Buffer* compare_buf = decoded_buf;
        Buffer* ref_compare_buf = cb->ref_buf_wrap;
        if (cb->decode_sws_ctx && cb->decode_converted_frame) {
            // 从 decoded_buf 获取 AVFrame
            auto* decoded_avfb = dynamic_cast<AVFrameBuffer*>(decoded_buf);
            AVFrame* decoded_avframe = decoded_avfb ? decoded_avfb->getAVFrame() : nullptr;
            if (decoded_avframe) {
                sws_scale(cb->decode_sws_ctx,
                    decoded_avframe->data, decoded_avframe->linesize,
                    0, decoded_avframe->height,
                    cb->decode_converted_frame->data, cb->decode_converted_frame->linesize);
                cb->decode_converted_frame->pts = decoded_avframe->pts;
                // 用转换后的帧做比较
                static thread_local AVFrameBuffer conv_wrap(
                    0, nullptr, 0, 0, Buffer::Ownership::EXTERNAL);
                conv_wrap.setAVFrame(cb->decode_converted_frame);
                compare_buf = &conv_wrap;
            }
        }

        if (cb->packed_101010 && cb->ref_rgb24_frame) {
            unpackPacked101010ToRgb24(
                cb->ref_avframe, cb->ref_rgb24_frame, cb->packed_101010_is_bgr);
            cb->ref_rgb24_frame->pts = cb->ref_avframe->pts;
            static thread_local AVFrameBuffer ref24_wrap(
                0, nullptr, 0, 0, Buffer::Ownership::EXTERNAL);
            ref24_wrap.setAVFrame(cb->ref_rgb24_frame);
            ref_compare_buf = &ref24_wrap;
        } else if (cb->subbyte_rgb_to_rgb24 && cb->ref_sws_ctx && cb->ref_rgb24_frame) {
            sws_scale(cb->ref_sws_ctx,
                cb->ref_avframe->data, cb->ref_avframe->linesize,
                0, cb->ref_avframe->height,
                cb->ref_rgb24_frame->data, cb->ref_rgb24_frame->linesize);
            cb->ref_rgb24_frame->pts = cb->ref_avframe->pts;
            static thread_local AVFrameBuffer ref24_wrap(
                0, nullptr, 0, 0, Buffer::Ownership::EXTERNAL);
            ref24_wrap.setAVFrame(cb->ref_rgb24_frame);
            ref_compare_buf = &ref24_wrap;
        }

        auto cmp = cb->compare_ctx->comparator_->compare(ref_compare_buf, compare_buf);
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

        if (!passed) {
            LOG4CPLUS_WARN_FMT(
                log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.test.venc.EncodeCompare")),
                "  frame: PSNR=%.2f (min=%.2f, %s), SSIM=%.4f (min=%.4f, %s) -> FAIL",
                psnr, cb->compare_ctx->min_psnr, psnr_ok ? "OK" : "FAIL",
                ssim, cb->compare_ctx->min_ssim, ssim_ok ? "OK" : "FAIL");
        }
        return true;
    };

    group.callback_chain.push_back(
        CallbackChainItem(callback, &cb_ctx, "venc_encode_quality_callback"));

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
        // ref_avframe 由 ref_buf_wrap RAII 管理，不可手动释放
        ref_source.close();
        return failResult(logger, "ENC_COMPARE", "Failed to start encode-decode pipeline");
    }
    pipeline_started = true;

    const uint64_t decoded_pool_id = pipeline->getGroupConsumerBufferPoolId(0, 0);
    auto decoded_pool = ComponentTopology::getInstance().getPool(decoded_pool_id).lock();
    if (!decoded_pool) {
        // ref_avframe 由 ref_buf_wrap RAII 管理，不可手动释放
        ref_source.close();
        return failResult(logger, "ENC_COMPARE", "Failed to lock decoded BufferPool");
    }

    // 关键：callback 做 compare，但我们仍需"drain"释放 filled buffer，避免 decode worker 卡在 acquireFree
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
    // 注意：ref_avframe 由 ref_buf_wrap 持有（setAVFrame），
    // 在函数返回时 ref_buf_wrap 析构会自动 av_frame_free。
    // 此处不可手动释放，否则 double-free → SIGSEGV。
    if (decode_sws_ctx) sws_freeContext(decode_sws_ctx);
    if (ref_sws_ctx) sws_freeContext(ref_sws_ctx);
    if (ref_scale_sws) sws_freeContext(ref_scale_sws);
    // 注意：decode_converted_frame / ref_rgb24_frame / ref_*avframe 在 callback 中被
    // AVFrameBuffer::setAVFrame() 持有，不可手动释放（避免 double-free）。
    (void)ref_rgb24_frame;
    (void)decode_converted_frame;
    (void)ref_avframe;
    (void)ref_file_avframe;

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
    encode_decode_cfg.consumer_type = ConsumerTypeConfigBuilder(encode_decode_cfg.consumer_type)
        .setCompareConfig(CompareConfigBuilder(encode_decode_cfg.consumer_type.compare)
            .setEnablePsnr(false)
            .setEnableSsim(false)
            .build())
        .build();

    MultiWorkerConfig multi_config;
    GroupConfig group("encode_decode_display_group");

    {
        WorkerConfig enc_wc = encode_decode_cfg;
        enc_wc.global.worker_type = WorkerType::FFMPEG_ENCODE;
        group.producers["encoder"] = enc_wc;
    }

    {
        WorkerConfig dec_wc = encode_decode_cfg;
        dec_wc.global.worker_type = WorkerType::FFMPEG_DECODE;
        dec_wc.data_source.buffer_mode = true;
        dec_wc.decoder.name = std::nullopt;
        dec_wc.decoder.enable_hardware = false;
        dec_wc.consumer_type = ConsumerTypeConfigBuilder(dec_wc.consumer_type)
            .setDisplayConfig(DisplayConsumerConfigBuilder(dec_wc.consumer_type.display)
                .setEnable(false)
                .build())
            .setSaveRawConfig(encode_decode_cfg.consumer_type.save_raw)
            .setSaveEncodedConfig(encode_decode_cfg.consumer_type.save_encoded)
            .build();
        group.consumers["decoder"] = dec_wc;
    }

    group.mode = GroupConfig::Mode::ONE_TO_ONE;

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
    decoded_pool = ComponentTopology::getInstance().getPool(decoded_pool_id).lock();
    if (!decoded_pool) {
        return failResult(logger, "ENC_DISPLAY", "Failed to lock decoded BufferPool");
    }

    uint32_t flags = test::ExecuteMode::buildConsumeFlags(shared_cfg);
    WorkerConfig consume_cfg = shared_cfg;
    if (consume_cfg.consumer_type.max_timeout_count < 50) {
        consume_cfg.consumer_type = ConsumerTypeConfigBuilder(consume_cfg.consumer_type)
            .setMaxTimeoutCount(50)
            .build();
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
