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
 *     ConsumerTypeConfigBuilder().setConsumerMaxFrames(300).build()  // 并入 setConsumerTypeConfig
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
            .setGlobalConfig(WorkerGlobalConfigBuilder().setWorkerType(WorkerType::FFMPEG_DECODE).build())
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
            .setGlobalConfig(WorkerGlobalConfigBuilder().setWorkerType(WorkerType::FFMPEG_DECODE).build())
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
            .setGlobalConfig(WorkerGlobalConfigBuilder().setWorkerType(WorkerType::FFMPEG_DECODE).build())
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
            .setGlobalConfig(WorkerGlobalConfigBuilder().setWorkerType(WorkerType::FFMPEG_DECODE).build())
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
    // PP（后处理）配置
    // ========================================
    
    /**
     * @brief 创建 PP0（通道0）YUV 格式配置
     * 
     * @param path 视频路径
     * @param format YUV 输出格式
     * @param width 输出宽度
     * @param height 输出高度
     * @param color_std 颜色标准
     */
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
            .setGlobalConfig(WorkerGlobalConfigBuilder().setWorkerType(WorkerType::FFMPEG_DECODE).build())
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
            .setGlobalConfig(WorkerGlobalConfigBuilder().setWorkerType(WorkerType::FFMPEG_DECODE).build())
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
            .setGlobalConfig(WorkerGlobalConfigBuilder().setWorkerType(WorkerType::FFMPEG_DECODE).build())
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
            .setGlobalConfig(WorkerGlobalConfigBuilder().setWorkerType(WorkerType::FFMPEG_DECODE).build())
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
            .setGlobalConfig(WorkerGlobalConfigBuilder().setWorkerType(WorkerType::FFMPEG_DECODE).build())
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
            .setGlobalConfig(WorkerGlobalConfigBuilder().setWorkerType(WorkerType::FFMPEG_PACKET_RECORDER).build())
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
