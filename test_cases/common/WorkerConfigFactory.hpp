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
