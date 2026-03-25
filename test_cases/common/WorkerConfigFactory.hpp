/**
 * @file WorkerConfigFactory.hpp
 * @brief WorkerConfig 快捷工厂
 * 
 * 提供便捷的 WorkerConfig 创建方法，简化测试代码。
 * 
 * 设计理念（参考 Google 风格）：
 * - 不定义新的配置结构，直接返回 WorkerConfig
 * - 提供常用配置的快捷创建方法
 * - 复用现有的 Builder 模式
 * - 职责分离：解码工厂只管 DataSource + Decoder + Global，
 *   显示配置由 DisplayPlugin 负责
 * 
 * 厂商扩展：
 * - 新增厂商只需在 vendorDecodeBuilders() 中注册一行
 * - codec 参数支持 ffmpeg 标准命名（如 h264_taco, hevc_nvidia）
 * 
 * @version 4.1
 */

#ifndef WORKER_CONFIG_FACTORY_HPP
#define WORKER_CONFIG_FACTORY_HPP

#include "productionline/worker/WorkerConfig.hpp"
#include "vendor/contracts/DecoderVendorExtension.hpp"
#include "vendor/taco/decode/TacoDecoderExtension.hpp"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace test {
namespace common {

/**
 * @brief 解码器名称解析结果
 *
 * 将 ffmpeg 风格的解码器名称（如 "h264_taco"）拆分为 codec 和 vendor。
 */
struct DecoderSpec {
    std::string codec;   ///< 编解码格式：h264, hevc, mjpeg
    std::string vendor;  ///< 厂商标识：taco, nvidia 等；空串表示软件解码
};

/**
 * @brief WorkerConfig 快捷工厂
 * 
 * 使用示例：
 * @code
 * // 默认厂商（taco）
 * auto config = WorkerConfigFactory::createDecode("/path/to/video.mp4", "h264");
 * 
 * // 显式指定厂商（ffmpeg 风格）
 * auto config = WorkerConfigFactory::createDecode("/path/to/video.mp4", "h264_nvidia");
 * 
 * // 软件解码
 * auto config = WorkerConfigFactory::createDecode("/path/to/video.mp4", "software");
 * @endcode
 */
class WorkerConfigFactory {
public:

    // ========================================
    // 厂商扩展注册
    // ========================================

    using VendorExtBuilder = std::function<std::unique_ptr<IDecoderVendorExtension>()>;

    /**
     * @brief 厂商解码器扩展构建分发表
     *
     * 每个厂商注册一个 lambda，负责创建"基础解码"用的 VendorExtension。
     * 新增厂商只需在此 map 中加一行。
     */
    static const std::unordered_map<std::string, VendorExtBuilder>& vendorDecodeBuilders() {
        static const std::unordered_map<std::string, VendorExtBuilder> map = {
            {"taco", []() -> std::unique_ptr<IDecoderVendorExtension> {
                auto taco = TacoConfigBuilder().setChannels(true, false).build();
                return makeTacoDecoderExtension(taco);
            }},
        };
        return map;
    }

    // ========================================
    // 解码器名称解析
    // ========================================

    /**
     * @brief 解析 ffmpeg 风格解码器名称
     *
     * | 输入            | 结果                          |
     * |-----------------|-------------------------------|
     * | "h264"          | {codec: "h264", vendor: "taco"} (默认厂商) |
     * | "h264_taco"     | {codec: "h264", vendor: "taco"} |
     * | "hevc_nvidia"   | {codec: "hevc", vendor: "nvidia"} |
     * | "software"/"sw" | {codec: "",     vendor: ""}     |
     */
    static DecoderSpec parseDecoderName(const std::string& decoder) {
        if (decoder == "software" || decoder == "sw") {
            return {"", ""};
        }

        static const std::unordered_map<std::string, std::string> aliases = {
            {"avc", "h264"},   {"h264", "h264"},
            {"hevc", "h265"},  {"h265", "h265"},
            {"mjpeg", "mjpeg"},{"jpeg", "mjpeg"},
        };

        auto pos = decoder.rfind('_');
        if (pos != std::string::npos) {
            std::string raw_codec = decoder.substr(0, pos);
            std::string vendor    = decoder.substr(pos + 1);

            auto it = aliases.find(raw_codec);
            return {it != aliases.end() ? it->second : raw_codec, vendor};
        }

        auto it = aliases.find(decoder);
        return {it != aliases.end() ? it->second : decoder, "taco"};
    }

    // ========================================
    // 通用解码配置（不含 Display，职责分离）
    // ========================================

    /**
     * @brief 创建通用解码配置
     *
     * 仅设置 DataSource + Decoder + Global，不设置 Display 配置。
     * 显示配置由 DisplayPlugin 独立负责。
     *
     * @param path 视频文件路径或 RTSP URL
     * @param decoder 解码器名称 (h264, h264_taco, hevc_nvidia, software)
     */
    static WorkerConfig createDecode(
        const std::string& path,
        const std::string& decoder
    ) {
        auto spec = parseDecoderName(decoder);

        if (spec.vendor.empty()) {
            return createSoftwareDecode(path);
        }

        return createHardwareDecode(path, spec);
    }

    /**
     * @brief 创建硬件解码配置（通用，厂商由 DecoderSpec 决定）
     */
    static WorkerConfig createHardwareDecode(
        const std::string& path,
        const DecoderSpec& spec
    ) {
        const auto& builders = vendorDecodeBuilders();
        auto it = builders.find(spec.vendor);
        if (it == builders.end()) {
            throw std::invalid_argument(
                "WorkerConfigFactory: unknown decoder vendor '" + spec.vendor + "'");
        }

        return WorkerConfigBuilder()
            .setDataSourceConfig(
                DataSourceConfigBuilder()
                    .setPath(path)
                    .setBufferCount(isRtspUrl(path) ? 8 : 128)
                    .build()
            )
            .setDecoderConfig(
                DecoderConfigBuilder()
                    .useVendor(spec.codec, it->second())
                    .build()
            )
            .setGlobalConfig(WorkerGlobalConfigBuilder().setWorkerType(WorkerType::FFMPEG_DECODE).build())
            .build();
    }

    // ========================================
    // 快捷方法（向后兼容，委托给 createHardwareDecode）
    // ========================================

    static WorkerConfig createH264Decode(const std::string& path) {
        return createHardwareDecode(path, {"h264", "taco"});
    }

    static WorkerConfig createH265Decode(const std::string& path) {
        return createHardwareDecode(path, {"h265", "taco"});
    }

    static WorkerConfig createMjpegDecode(const std::string& path) {
        return createHardwareDecode(path, {"mjpeg", "taco"});
    }

    /**
     * @brief 创建软件解码配置
     */
    static WorkerConfig createSoftwareDecode(const std::string& path) {
        return WorkerConfigBuilder()
            .setDataSourceConfig(
                DataSourceConfigBuilder()
                    .setPath(path)
                    .setBufferCount(128)
                    .build()
            )
            .setDecoderConfig(
                DecoderConfigBuilder()
                    .useSoftware()
                    .build()
            )
            .setGlobalConfig(WorkerGlobalConfigBuilder().setWorkerType(WorkerType::FFMPEG_DECODE).build())
            .build();
    }

    // ========================================
    // 视频编码配置（YUV/RGB 文件 → 硬件/软件编码）
    // ========================================
    
    /**
     * @brief 创建 H.264 硬件编码配置（RawFrameSourceFromFile）
     */
    static WorkerConfig createH264Encode(
        const std::string& input_path,
        int output_width = 1920,
        int output_height = 1080,
        int bitrate_kbps = 4000,
        double fps = 30.0,
        int gop_size = 30,
        int profile = 77,
        int input_pix_fmt = 23)
    {
        WorkerConfig::EncoderConfig encoder_config;
        encoder_config.name = std::optional<std::string>("h264_taco");
        encoder_config.enable_hardware = true;
        encoder_config.bit_rate = bitrate_kbps > 0 ? bitrate_kbps * 1000 : 0;
        encoder_config.gop_size = gop_size;
        encoder_config.framerate_num = static_cast<int>(fps);
        encoder_config.framerate_den = 1;
        encoder_config.input_pix_fmt = input_pix_fmt;
        encoder_config.rc_mode = 1;
        encoder_config.taco.profile = profile;
        
        WorkerConfig config = WorkerConfigBuilder()
            .setDataSourceConfig(
                DataSourceConfigBuilder()
                    .setPath(input_path)
                    .setBufferCount(32)
                    .build()
            )
            .setDisplayConfig(
                DisplayConfigBuilder()
                    .setDisplayResolution(output_width, output_height)
                    .setBitsPerPixel(32)
                    .build()
            )
            .setGlobalConfig(WorkerGlobalConfigBuilder().setWorkerType(WorkerType::FFMPEG_ENCODE).build())
            .build();
        config.encoder = encoder_config;
        return config;
    }
    
    static WorkerConfig createH265Encode(
        const std::string& input_path,
        int output_width = 1920,
        int output_height = 1080,
        int bitrate_kbps = 4000,
        double fps = 30.0,
        int gop_size = 30,
        int profile = 1,
        int input_pix_fmt = 23)
    {
        WorkerConfig::EncoderConfig encoder_config;
        encoder_config.name = std::optional<std::string>("hevc_taco");
        encoder_config.enable_hardware = true;
        encoder_config.bit_rate = bitrate_kbps > 0 ? bitrate_kbps * 1000 : 0;
        encoder_config.gop_size = gop_size;
        encoder_config.framerate_num = static_cast<int>(fps);
        encoder_config.framerate_den = 1;
        encoder_config.input_pix_fmt = input_pix_fmt;
        encoder_config.rc_mode = 1;
        encoder_config.taco.profile = profile;
        
        WorkerConfig config = WorkerConfigBuilder()
            .setDataSourceConfig(
                DataSourceConfigBuilder()
                    .setPath(input_path)
                    .setBufferCount(32)
                    .build()
            )
            .setDisplayConfig(
                DisplayConfigBuilder()
                    .setDisplayResolution(output_width, output_height)
                    .setBitsPerPixel(32)
                    .build()
            )
            .setGlobalConfig(WorkerGlobalConfigBuilder().setWorkerType(WorkerType::FFMPEG_ENCODE).build())
            .build();
        config.encoder = encoder_config;
        return config;
    }
    
    static WorkerConfig createJpegEncode(
        const std::string& input_path,
        int output_width = 1920,
        int output_height = 1080,
        int quality = 80,
        double fps = 25.0,
        int input_pix_fmt = 23)
    {
        WorkerConfig::EncoderConfig encoder_config;
        encoder_config.name = std::optional<std::string>("jpeg_taco");
        encoder_config.enable_hardware = true;
        encoder_config.framerate_num = static_cast<int>(fps);
        encoder_config.framerate_den = 1;
        encoder_config.input_pix_fmt = input_pix_fmt;
        encoder_config.jpeg.quality = quality;
        
        WorkerConfig config = WorkerConfigBuilder()
            .setDataSourceConfig(
                DataSourceConfigBuilder()
                    .setPath(input_path)
                    .setBufferCount(32)
                    .build()
            )
            .setDisplayConfig(
                DisplayConfigBuilder()
                    .setDisplayResolution(output_width, output_height)
                    .setBitsPerPixel(32)
                    .build()
            )
            .setGlobalConfig(WorkerGlobalConfigBuilder().setWorkerType(WorkerType::FFMPEG_ENCODE).build())
            .build();
        config.encoder = encoder_config;
        return config;
    }
    
    static WorkerConfig createSoftwareEncode(
        const std::string& input_path,
        const std::string& codec,
        int output_width = 1920,
        int output_height = 1080,
        int bitrate_kbps = 4000,
        double fps = 30.0,
        int gop_size = 30,
        int input_pix_fmt = 23)
    {
        WorkerConfig::EncoderConfig encoder_config;
        if (codec == "h264" || codec == "avc") {
            encoder_config.name = std::optional<std::string>("libx264");
        } else if (codec == "h265" || codec == "hevc") {
            encoder_config.name = std::optional<std::string>("libx265");
        } else {
            encoder_config.name = std::optional<std::string>("libx264");
        }
        encoder_config.enable_hardware = false;
        encoder_config.bit_rate = bitrate_kbps > 0 ? bitrate_kbps * 1000 : 0;
        encoder_config.gop_size = gop_size;
        encoder_config.framerate_num = static_cast<int>(fps);
        encoder_config.framerate_den = 1;
        encoder_config.input_pix_fmt = input_pix_fmt;
        encoder_config.rc_mode = 1;
        
        WorkerConfig config = WorkerConfigBuilder()
            .setDataSourceConfig(
                DataSourceConfigBuilder()
                    .setPath(input_path)
                    .setBufferCount(32)
                    .build()
            )
            .setDisplayConfig(
                DisplayConfigBuilder()
                    .setDisplayResolution(output_width, output_height)
                    .setBitsPerPixel(32)
                    .build()
            )
            .setGlobalConfig(WorkerGlobalConfigBuilder().setWorkerType(WorkerType::FFMPEG_ENCODE).build())
            .build();
        config.encoder = encoder_config;
        return config;
    }
    
    static WorkerConfig createEncode(
        const std::string& input_path,
        const std::string& encoder,
        int output_width = 1920,
        int output_height = 1080,
        int bitrate_kbps = 4000,
        double fps = 30.0)
    {
        if (encoder == "h264_taco") {
            return createH264Encode(input_path, output_width, output_height, bitrate_kbps, fps);
        }
        if (encoder == "hevc_taco" || encoder == "h265_taco") {
            return createH265Encode(input_path, output_width, output_height, bitrate_kbps, fps);
        }
        if (encoder == "jpeg_taco") {
            return createJpegEncode(input_path, output_width, output_height, 80, fps);
        }
        if (encoder == "libx264" || encoder == "sw_h264") {
            return createSoftwareEncode(input_path, "h264", output_width, output_height, bitrate_kbps, fps);
        }
        if (encoder == "libx265" || encoder == "sw_h265") {
            return createSoftwareEncode(input_path, "h265", output_width, output_height, bitrate_kbps, fps);
        }
        return createH264Encode(input_path, output_width, output_height, bitrate_kbps, fps);
    }

    // ========================================
    // OpenCV 操作配置
    // ========================================

    /**
     * @brief 从参数字符串构建 WorkerConfig（含 OpencvType 配置）
     *
     * 参数字符串格式（以 '_' 分隔）：
     *   "<opencv_op>_<param1>_<param2>..."
     *   - resize   : param1=dst_width, param2=dst_height
     *   - crop     : param1=width, param2=height
     *   - erode/dilate/open/close : param1=kernel_size, param2=iterations
     *   - sobel    : param1=dx, param2=dy, param3=ksize
     *   - canny    : param1=threshold1, param2=threshold2
     *   - laplacian: param1=ksize
     *   - translate: param1=tx, param2=ty
     *   - rotate   : param1=angle, param2=scale
     *   - perspective: param1=offset
     *   - line     : param1=x1, param2=y1, param3=x2, param4=y2
     *   - rectangle: param1=x, param2=y, param3=width, param4=height
     *   - puttext  : param1=x, param2=y
     *   - blur     : param1=ksize, param2=sigma_x
     *   - threshold: param1=thresh, param2=maxval
     *
     * @param path        视频文件路径或 RTSP URL
     * @param params_str  参数字符串，例如 "resize_1280_720" 或 "canny_100_200"
     * @param use_hardware 是否使用硬件解码（默认 true，使用 h264_taco）
     * @return 完整的 WorkerConfig，consumer_type.opencv 已按参数填好
     */
    static WorkerConfig buildOpencvConfig(
        const std::string& path,
        const std::string& params_str,
        bool use_hardware = true
    ) {
        using OpType = WorkerConfig::ConsumerTypeConfig::OpencvType::OpType;

        // 按 '_' 分割参数字符串
        std::vector<std::string> fields;
        std::istringstream ss(params_str);
        std::string token;
        while (std::getline(ss, token, '_')) {
            fields.push_back(token);
        }

        OpType op = OpType::NONE;

        if (!fields.empty()) {
            if      (fields[0] == "resize")      op = OpType::RESIZE;
            else if (fields[0] == "crop")        op = OpType::CROP;
            else if (fields[0] == "erode")       op = OpType::ERODE;
            else if (fields[0] == "dilate")      op = OpType::DILATE;
            else if (fields[0] == "open")        op = OpType::MORPH_OPEN;
            else if (fields[0] == "close")       op = OpType::MORPH_CLOSE;
            else if (fields[0] == "sobel")       op = OpType::SOBEL;
            else if (fields[0] == "canny")       op = OpType::CANNY;
            else if (fields[0] == "laplacian")   op = OpType::LAPLACIAN;
            else if (fields[0] == "translate")   op = OpType::TRANSLATE;
            else if (fields[0] == "rotate")      op = OpType::ROTATE;
            else if (fields[0] == "perspective") op = OpType::PERSPECTIVE;
            else if (fields[0] == "line")        op = OpType::DRAW_LINE;
            else if (fields[0] == "rectangle")   op = OpType::DRAW_RECT;
            else if (fields[0] == "puttext")     op = OpType::PUT_TEXT;
            else if (fields[0] == "blur")        op = OpType::GAUSSIAN_BLUR;
            else if (fields[0] == "threshold")   op = OpType::THRESHOLD;
            else if (fields[0] == "split")       op = OpType::SPLIT;
            else if (fields[0] == "merge")       op = OpType::MERGE;
            else if (fields[0] == "cvtcolor")    op = OpType::CVTCOLOR;
            else if (fields[0] == "add")         op = OpType::ADD;
            else if (fields[0] == "absdiff")     op = OpType::ABSDIFF;
            else if (fields[0] == "addweighted") op = OpType::ADD_WEIGHTED;
            else if (fields[0] == "bitwiseand")  op = OpType::BITWISE_AND;
            else if (fields[0] == "bitwiseor")   op = OpType::BITWISE_OR;
            else if (fields[0] == "bitwisexor")  op = OpType::BITWISE_XOR;
            else if (fields[0] == "bitwisenot")  op = OpType::BITWISE_NOT;
            else if (fields[0] == "saveloadimg") op = OpType::SAVE_LOAD_IMG;
        }

        // 辅助函数：安全读取第 idx 个字段
        auto getI = [&](size_t idx, int def) -> int {
            return (idx < fields.size()) ? std::stoi(fields[idx]) : def;
        };
        auto getD = [&](size_t idx, double def) -> double {
            return (idx < fields.size()) ? std::stod(fields[idx]) : def;
        };

        const std::string decoder = use_hardware ? "h264" : "software";
        auto config = createDecode(path, decoder);
        config.consumer_type.compare.enable_psnr = true;
        config.consumer_type.compare.min_psnr = 1.0;
        config.consumer_type.compare.enable_ssim = true;
        config.consumer_type.compare.min_ssim = 1.0;

        auto& opencv = config.consumer_type.opencv;
        if (op != OpType::NONE) {
            opencv.enable  = true;
            opencv.op_type = op;
            if (op == OpType::RESIZE) {
                opencv.resize.dst_width     = getI(1, 0);
                opencv.resize.dst_height    = getI(2, 0);
                opencv.resize.fx            = 0.0;
                opencv.resize.fy            = 0.0;
                opencv.resize.interpolation = 1;
            } else if (op == OpType::CROP) {
                opencv.crop.x      = 0;
                opencv.crop.y      = 0;
                opencv.crop.width  = getI(1, 0);
                opencv.crop.height = getI(2, 0);
            } else if (op == OpType::ERODE  || op == OpType::DILATE ||
                       op == OpType::MORPH_OPEN || op == OpType::MORPH_CLOSE) {
                opencv.morph.kernel_size = getI(1, 3);
                opencv.morph.iterations  = getI(2, 1);
            } else if (op == OpType::SOBEL) {
                opencv.sobel.dx    = getI(1, 1);
                opencv.sobel.dy    = getI(2, 0);
                opencv.sobel.ksize = getI(3, 3);
            } else if (op == OpType::CANNY) {
                opencv.canny.threshold1 = getD(1, 100.0);
                opencv.canny.threshold2 = getD(2, 200.0);
            } else if (op == OpType::LAPLACIAN) {
                opencv.laplacian.ksize = getI(1, 1);
            } else if (op == OpType::TRANSLATE) {
                opencv.translate.tx = getD(1, 0.0);
                opencv.translate.ty = getD(2, 0.0);
            } else if (op == OpType::ROTATE) {
                opencv.rotate.angle = getD(1, 0.0);
                opencv.rotate.scale = getD(2, 1.0);
            } else if (op == OpType::PERSPECTIVE) {
                opencv.perspective.offset = getI(1, 50);
            } else if (op == OpType::DRAW_LINE) {
                opencv.draw_line.x1 = getI(1, 0);
                opencv.draw_line.y1 = getI(2, 0);
                opencv.draw_line.x2 = getI(3, 100);
                opencv.draw_line.y2 = getI(4, 100);
            } else if (op == OpType::DRAW_RECT) {
                opencv.draw_rect.x      = getI(1, 100);
                opencv.draw_rect.y      = getI(2, 100);
                opencv.draw_rect.width  = getI(3, 200);
                opencv.draw_rect.height = getI(4, 200);
            } else if (op == OpType::PUT_TEXT) {
                opencv.put_text.x = getI(1, 10);
                opencv.put_text.y = getI(2, 50);
            } else if (op == OpType::GAUSSIAN_BLUR) {
                opencv.gaussian_blur.ksize   = getI(1, 5);
                opencv.gaussian_blur.sigma_x = getD(2, 0.0);
            } else if (op == OpType::THRESHOLD) {
                opencv.threshold.thresh = getD(1, 128.0);
                opencv.threshold.maxval = getD(2, 255.0);
            } else if (op == OpType::SPLIT || op == OpType::MERGE) {
                opencv.split_merge.channels = getI(1, 3);  // 默认 3 通道
            } else if (op == OpType::CVTCOLOR) {
                // cvtcolor_<code>[_<dstCn>]
                // code 取值：40=YUV2BGR_NV12, 91=YUV2GRAY_NV12, 116=YUV2RGB_NV12, 等
                // 完整列表见 opencv2/imgproc.hpp
                opencv.cvtcolor.code = getI(1, 40);   // 默认 CV_YUV2BGR_NV12
                opencv.cvtcolor.dstCn = getI(2, 0);   // 0=自动
            } else if (op == OpType::ADD || op == OpType::ABSDIFF ||
                       op == OpType::ADD_WEIGHTED || op == OpType::BITWISE_AND ||
                       op == OpType::BITWISE_OR || op == OpType::BITWISE_XOR ||
                       op == OpType::BITWISE_NOT) {
                // 多生产者算术/逻辑运算，不需要额外参数
                // 通过 enable_psnr/enable_ssim 控制质量评估
                if (fields.size() > 1 && fields[1] == "psnr") {
                    opencv.enable_psnr = true;
                    opencv.enable_ssim = false;
                } else if (fields.size() > 1 && fields[1] == "ssim") {
                    opencv.enable_psnr = false;
                    opencv.enable_ssim = true;
                }
            } else if (op == OpType::SAVE_LOAD_IMG) {
                // 图片保存/读取 I/O 测试，单生产者模式
                // 通过 enable_psnr/enable_ssim 控制质量评估
                if (fields.size() > 1 && fields[1] == "psnr") {
                    opencv.enable_psnr = true;
                    opencv.enable_ssim = false;
                } else if (fields.size() > 1 && fields[1] == "ssim") {
                    opencv.enable_psnr = false;
                    opencv.enable_ssim = true;
                }
            }
        }

        return config;
    }

    /**
     * @brief 从参数字符串构建 WorkerConfig（含 OpencvType 配置）- 支持 split/merge 测试
     *
     * split/merge 测试逻辑：
     *   硬件 Mat -> split -> merge -> 与软件 Mat 比较
     *
     * 参数字符串格式：
     *   - split      : 通道分离测试（默认 3 通道）
     *   - split_3    : 3 通道分离测试
     *   - split_4    : 4 通道分离测试
     *   - merge      : 通道合并测试（默认 3 通道）
     *   - merge_3    : 3 通道合并测试
     *   - merge_4    : 4 通道合并测试
     *
     * @param path        视频文件路径或 RTSP URL
     * @param params_str  参数字符串，例如 "split" 或 "merge_4"
     * @param use_hardware 是否使用硬件解码（默认 true）
     * @return 完整的 WorkerConfig
     */
    static WorkerConfig buildSplitMergeConfig(
        const std::string& path,
        const std::string& params_str,
        bool use_hardware = true
    ) {
        return buildOpencvConfig(path, params_str, use_hardware);
    }

    // ========================================
    // PP（后处理）配置
    // PP 的 width/height 是输出分辨率，属于解码后处理范畴
    // ========================================

    static WorkerConfig createPP0YuvConfig(
        const std::string& path,
        OutputFormat format,
        int width = 1920,
        int height = 1080,
        ColorStandard color_std = ColorStandard::BT601
    ) {
        auto taco = TacoConfigBuilder()
            .setChannels(true, false)
            .setOutputFormat(Channel::CH0, format, color_std)
            .setScale(Channel::CH0, width, height)
            .build();
        
        return WorkerConfigBuilder()
            .setDataSourceConfig(
                DataSourceConfigBuilder()
                    .setPath(path)
                    .setBufferCount(128)
                    .build()
            )
            .setDecoderConfig(
                DecoderConfigBuilder()
                    .useTaco("h264", taco)
                    .build()
            )
            .setGlobalConfig(WorkerGlobalConfigBuilder().setWorkerType(WorkerType::FFMPEG_DECODE).build())
            .build();
    }
    
    static WorkerConfig createPP1RgbConfig(
        const std::string& path,
        OutputFormat format,
        int width = 1920,
        int height = 1080,
        ColorStandard color_std = ColorStandard::BT601
    ) {
        auto taco = TacoConfigBuilder()
            .setChannels(false, true)
            .setOutputFormat(Channel::CH1, format, color_std)
            .setScale(Channel::CH1, width, height)
            .build();
        
        return WorkerConfigBuilder()
            .setDataSourceConfig(
                DataSourceConfigBuilder()
                    .setPath(path)
                    .setBufferCount(128)
                    .build()
            )
            .setDecoderConfig(
                DecoderConfigBuilder()
                    .useTaco("h264", taco)
                    .build()
            )
            .setGlobalConfig(WorkerGlobalConfigBuilder().setWorkerType(WorkerType::FFMPEG_DECODE).build())
            .build();
    }
    
    static WorkerConfig createPP1YuvConfig(
        const std::string& path,
        OutputFormat format,
        int width = 1920,
        int height = 1080,
        ColorStandard color_std = ColorStandard::BT601
    ) {
        auto taco = TacoConfigBuilder()
            .setChannels(false, true)
            .setOutputFormat(Channel::CH1, format, color_std)
            .setScale(Channel::CH1, width, height)
            .build();
        
        return WorkerConfigBuilder()
            .setDataSourceConfig(
                DataSourceConfigBuilder()
                    .setPath(path)
                    .setBufferCount(128)
                    .build()
            )
            .setDecoderConfig(
                DecoderConfigBuilder()
                    .useTaco("h264", taco)
                    .build()
            )
            .setGlobalConfig(WorkerGlobalConfigBuilder().setWorkerType(WorkerType::FFMPEG_DECODE).build())
            .build();
    }
    
    static WorkerConfig createMultiPPConfig(
        const std::string& path,
        OutputFormat pp0_format,
        OutputFormat pp1_format,
        int width = 1920,
        int height = 1080,
        ColorStandard color_std = ColorStandard::BT601
    ) {
        auto taco = TacoConfigBuilder()
            .setChannels(true, true)
            .setOutputFormat(Channel::CH0, pp0_format, color_std)
            .setOutputFormat(Channel::CH1, pp1_format, color_std)
            .setScale(Channel::CH0, width, height)
            .setScale(Channel::CH1, width, height)
            .build();
        
        return WorkerConfigBuilder()
            .setDataSourceConfig(
                DataSourceConfigBuilder()
                    .setPath(path)
                    .setBufferCount(128)
                    .build()
            )
            .setDecoderConfig(
                DecoderConfigBuilder()
                    .useTaco("h264", taco)
                    .build()
            )
            .setGlobalConfig(WorkerGlobalConfigBuilder().setWorkerType(WorkerType::FFMPEG_DECODE).build())
            .build();
    }
    
    static WorkerConfig createCropConfig(
        const std::string& path,
        int crop_x, int crop_y, int crop_w, int crop_h,
        int scale_w, int scale_h
    ) {
        auto taco = TacoConfigBuilder()
            .setChannels(true, false)
            .setCrop(Channel::CH0, crop_x, crop_y, crop_w, crop_h)
            .setScale(Channel::CH0, scale_w, scale_h)
            .build();
        
        return WorkerConfigBuilder()
            .setDataSourceConfig(
                DataSourceConfigBuilder()
                    .setPath(path)
                    .setBufferCount(128)
                    .build()
            )
            .setDecoderConfig(
                DecoderConfigBuilder()
                    .useTaco("h264", taco)
                    .build()
            )
            .setGlobalConfig(WorkerGlobalConfigBuilder().setWorkerType(WorkerType::FFMPEG_DECODE).build())
            .build();
    }
    
    // ========================================
    // RTSP 配置
    // ========================================
    
    static WorkerConfig createRtspDecode(
        const std::string& rtsp_url,
        const std::string& decoder = "h264"
    ) {
        auto config = createDecode(rtsp_url, decoder);
        config.data_source.buffer_count = 8;
        return config;
    }
    
    static WorkerConfig createRtspRecord(
        const std::string& rtsp_url,
        int buffer_count = 64
    ) {
        return WorkerConfigBuilder()
            .setDataSourceConfig(
                DataSourceConfigBuilder()
                    .setPath(rtsp_url)
                    .setBufferCount(buffer_count)
                    .build()
            )
            .setGlobalConfig(WorkerGlobalConfigBuilder().setWorkerType(WorkerType::FFMPEG_PACKET_RECORDER).build())
            .build();
    }
    
    // ========================================
    // 辅助方法
    // ========================================
    
    static bool isRtspUrl(const std::string& path) {
        return path.find("rtsp://") == 0 || 
               path.find("rtsps://") == 0;
    }
    
    static bool isStreamUrl(const std::string& path) {
        return path.find("rtsp://") == 0 ||
               path.find("rtsps://") == 0 ||
               path.find("http://") == 0 ||
               path.find("https://") == 0;
    }
};

} // namespace common
} // namespace test

#endif // WORKER_CONFIG_FACTORY_HPP
