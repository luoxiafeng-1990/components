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
 * 
 * @version 3.1
 */

#ifndef WORKER_CONFIG_FACTORY_HPP
#define WORKER_CONFIG_FACTORY_HPP

#include "productionline/worker/WorkerConfig.hpp"
#include <string>
#include <sstream>
#include <vector>

namespace test {
namespace common {

/**
 * @brief WorkerConfig 快捷工厂
 * 
 * 使用示例：
 * @code
 * // 创建 H264 硬件解码配置
 * auto config = WorkerConfigFactory::createH264Decode("/path/to/video.mp4");
 * 
 * // 创建带测试参数的配置
 * auto config = WorkerConfigFactory::createH264Decode("/path/to/video.mp4")
 *     .setMaxFrames(300)
 *     .setSaveFrames(-1)
 *     .setTargetFps(30.0);
 * @endcode
 */
class WorkerConfigFactory {
public:
    // ========================================
    // 视频解码配置
    // ========================================
    
    /**
     * @brief 创建 H264 硬件解码配置
     * 
     * @param path 视频文件路径或 RTSP URL
     * @param width 显示宽度（默认 1920）
     * @param height 显示高度（默认 1080）
     * @return WorkerConfig 配置对象
     */
    static WorkerConfig createH264Decode(
        const std::string& path,
        int width = 1920,
        int height = 1080
    ) {
        auto taco = TacoConfigBuilder()
            .setChannels(true, false)
            .build();
        
        return WorkerConfigBuilder()
            .setDataSourceConfig(
                DataSourceConfigBuilder()
                    .setPath(path)
                    .setBufferCount(isRtspUrl(path) ? 8 : 128)
                    .build()
            )
            .setDisplayConfig(
                DisplayConfigBuilder()
                    .setDisplayResolution(width, height)
                    .setBitsPerPixel(32)
                    .build()
            )
            .setDecoderConfig(
                DecoderConfigBuilder()
                    .useTaco("h264", taco)
                    .build()
            )
            .setWorkerType(WorkerType::FFMPEG_DECODE)
            .build();
    }
    
    /**
     * @brief 创建 H265/HEVC 硬件解码配置
     */
    static WorkerConfig createH265Decode(
        const std::string& path,
        int width = 1920,
        int height = 1080
    ) {
        auto taco = TacoConfigBuilder()
            .setChannels(true, false)
            .build();
        
        return WorkerConfigBuilder()
            .setDataSourceConfig(
                DataSourceConfigBuilder()
                    .setPath(path)
                    .setBufferCount(isRtspUrl(path) ? 8 : 128)
                    .build()
            )
            .setDisplayConfig(
                DisplayConfigBuilder()
                    .setDisplayResolution(width, height)
                    .setBitsPerPixel(32)
                    .build()
            )
            .setDecoderConfig(
                DecoderConfigBuilder()
                    .useTaco("hevc", taco)
                    .build()
            )
            .setWorkerType(WorkerType::FFMPEG_DECODE)
            .build();
    }
    
    /**
     * @brief 创建 MJPEG 硬件解码配置
     */
    static WorkerConfig createMjpegDecode(
        const std::string& path,
        int width = 1920,
        int height = 1080
    ) {
        auto taco = TacoConfigBuilder()
            .setChannels(true, false)
            .build();
        
        return WorkerConfigBuilder()
            .setDataSourceConfig(
                DataSourceConfigBuilder()
                    .setPath(path)
                    .setBufferCount(isRtspUrl(path) ? 8 : 128)
                    .build()
            )
            .setDisplayConfig(
                DisplayConfigBuilder()
                    .setDisplayResolution(width, height)
                    .setBitsPerPixel(32)
                    .build()
            )
            .setDecoderConfig(
                DecoderConfigBuilder()
                    .useTaco("mjpeg", taco)
                    .build()
            )
            .setWorkerType(WorkerType::FFMPEG_DECODE)
            .build();
    }
    
    /**
     * @brief 创建软件解码配置
     */
    static WorkerConfig createSoftwareDecode(
        const std::string& path,
        int width = 1920,
        int height = 1080
    ) {
        return WorkerConfigBuilder()
            .setDataSourceConfig(
                DataSourceConfigBuilder()
                    .setPath(path)
                    .setBufferCount(128)
                    .build()
            )
            .setDisplayConfig(
                DisplayConfigBuilder()
                    .setDisplayResolution(width, height)
                    .setBitsPerPixel(32)
                    .build()
            )
            .setDecoderConfig(
                DecoderConfigBuilder()
                    .useSoftware()
                    .build()
            )
            .setWorkerType(WorkerType::FFMPEG_DECODE)
            .build();
    }
    
    /**
     * @brief 创建通用解码配置（根据解码器名称）
     * 
     * @param path 视频路径
     * @param decoder 解码器名称 (h264_taco, hevc_taco, mjpeg_taco, software)
     * @param width 显示宽度
     * @param height 显示高度
     */
    static WorkerConfig createDecode(
        const std::string& path,
        const std::string& decoder,
        int width = 1920,
        int height = 1080
    ) {
        if (decoder == "software" || decoder == "sw") {
            return createSoftwareDecode(path, width, height);
        }
        
        // 解析解码器名称: codec_taco -> codec
        std::string codec = decoder;
        size_t pos = decoder.find("_taco");
        if (pos != std::string::npos) {
            codec = decoder.substr(0, pos);
        }
        
        // 映射常见名称
        if (codec == "h264" || codec == "avc") {
            return createH264Decode(path, width, height);
        } else if (codec == "h265" || codec == "hevc") {
            return createH265Decode(path, width, height);
        } else if (codec == "mjpeg" || codec == "jpeg") {
            return createMjpegDecode(path, width, height);
        }
        
        // 默认 H264
        return createH264Decode(path, width, height);
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
            .setDisplayConfig(
                DisplayConfigBuilder()
                    .setDisplayResolution(width, height)
                    .setBitsPerPixel(32)
                    .build()
            )
            .setDecoderConfig(
                DecoderConfigBuilder()
                    .useTaco("h264", taco)
                    .build()
            )
            .setWorkerType(WorkerType::FFMPEG_DECODE)
            .build();
    }
    
    /**
     * @brief 创建 PP1（通道1）RGB 格式配置
     * 
     * @param path 视频路径
     * @param format RGB 输出格式
     * @param width 输出宽度
     * @param height 输出高度
     * @param color_std 颜色标准
     */
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
            .setDisplayConfig(
                DisplayConfigBuilder()
                    .setDisplayResolution(width, height)
                    .setBitsPerPixel(32)
                    .build()
            )
            .setDecoderConfig(
                DecoderConfigBuilder()
                    .useTaco("h264", taco)
                    .build()
            )
            .setWorkerType(WorkerType::FFMPEG_DECODE)
            .build();
    }
    
    /**
     * @brief 创建 PP1（通道1）YUV 格式配置
     */
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
            .setDisplayConfig(
                DisplayConfigBuilder()
                    .setDisplayResolution(width, height)
                    .setBitsPerPixel(32)
                    .build()
            )
            .setDecoderConfig(
                DecoderConfigBuilder()
                    .useTaco("h264", taco)
                    .build()
            )
            .setWorkerType(WorkerType::FFMPEG_DECODE)
            .build();
    }
    
    /**
     * @brief 创建 Multi-PP 配置（PP0 + PP1 同时输出）
     * 
     * @param path 视频路径
     * @param pp0_format PP0 输出格式（YUV）
     * @param pp1_format PP1 输出格式（RGB 或 YUV）
     * @param width 输出宽度
     * @param height 输出高度
     */
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
            .setDisplayConfig(
                DisplayConfigBuilder()
                    .setDisplayResolution(width, height)
                    .setBitsPerPixel(32)
                    .build()
            )
            .setDecoderConfig(
                DecoderConfigBuilder()
                    .useTaco("h264", taco)
                    .build()
            )
            .setWorkerType(WorkerType::FFMPEG_DECODE)
            .build();
    }
    
    /**
     * @brief 创建带裁剪的 PP 配置
     * 
     * @param path 视频路径
     * @param crop_x 裁剪起始 X
     * @param crop_y 裁剪起始 Y
     * @param crop_w 裁剪宽度
     * @param crop_h 裁剪高度
     * @param scale_w 缩放后宽度
     * @param scale_h 缩放后高度
     */
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
            .setDisplayConfig(
                DisplayConfigBuilder()
                    .setDisplayResolution(scale_w, scale_h)
                    .setBitsPerPixel(32)
                    .build()
            )
            .setDecoderConfig(
                DecoderConfigBuilder()
                    .useTaco("h264", taco)
                    .build()
            )
            .setWorkerType(WorkerType::FFMPEG_DECODE)
            .build();
    }
    
    // ========================================
    // RTSP 配置
    // ========================================
    
    /**
     * @brief 创建 RTSP 解码配置
     * 
     * @param rtsp_url RTSP URL
     * @param decoder 解码器名称
     * @param width 显示宽度
     * @param height 显示高度
     */
    static WorkerConfig createRtspDecode(
        const std::string& rtsp_url,
        const std::string& decoder = "h264",
        int width = 1920,
        int height = 1080
    ) {
        auto config = createDecode(rtsp_url, decoder, width, height);
        config.data_source.buffer_count = 8; // RTSP 使用更小的缓冲区
        return config;
    }
    
    /**
     * @brief 创建 RTSP 录制配置
     */
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
            .setWorkerType(WorkerType::FFMPEG_PACKET_RECORDER)
            .build();
    }
    
    // ========================================
    // 辅助方法
    // ========================================
    
    /**
     * @brief 检查是否是 RTSP URL
     */
    static bool isRtspUrl(const std::string& path) {
        return path.find("rtsp://") == 0 || 
               path.find("rtsps://") == 0;
    }
    
    /**
     * @brief 检查是否是流媒体 URL
     */
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
