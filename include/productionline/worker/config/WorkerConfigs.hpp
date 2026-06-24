#ifndef WORKER_CONFIGS_HPP
#define WORKER_CONFIGS_HPP

#include <string>
#include <string_view>
#include <optional>
#include <memory>
#include <vector>
#include <map>
#include <functional>
#include "common/Logger.hpp"
#include "vendor/contracts/DecoderVendorExtension.hpp"
#include "vendor/contracts/DisplayVendorExtension.hpp"
#include "vendor/contracts/EncoderVendorExtension.hpp"
#include "consumptionline/config/ConsumerTypeConfig.hpp"

// FFmpeg 头文件（用于 AVRational 和 AVCodecParameters）
extern "C" {
#include <libavutil/rational.h>
}

// 前向声明（避免循环依赖）
class IEncodedPacketSource;
class IRawFrameSource;

/**
 * @brief Worker 类型枚举
 * 
 * 注意：此枚举独立定义，避免与 WorkerFactory 的循环依赖
 * 
 * v3.0 重构：
 * - FFMPEG_DECODE: 统一的解码 Worker（支持文件/RTSP/Buffer 模式）
 * - FFMPEG_PACKET_RECORDER: 录制 Worker
 */
enum class WorkerType {
    AUTO,                   // 自动检测（默认）
    FFMPEG_DECODE,          // FFmpeg 解码 Worker（统一处理文件和 RTSP 流）
    FFMPEG_PACKET_RECORDER,  // FFmpeg Packet 录制器（支持 RTSP/文件/HTTP 等多种数据源）
    FFMPEG_ENCODE,          // FFmpeg 编码 Worker（H.264/H.265/JPEG 编码）
    OPENCV                  // OpenCV Worker（图像处理操作）
};

/**
 * @brief Worker 配置（完整版）
 * 
 * 设计理念：
 * - 完全独立：包含 Worker 需要的所有配置
 * - 配置分离：数据源、显示设备、解码器配置独立
 * - Builder 构建：链式调用，易用易读
 * - 厂商解耦：厂商特有参数通过 vendor extension 接口注入，核心配置保持通用
 */
struct WorkerConfig {
    // ========================================
    // 数据源配置
    // ========================================
    struct DataSourceConfig {
        /// Worker 默认的 BufferPool Buffer 数量（当 buffer_count == 0 时使用）
        static constexpr int kDefaultBufferCount = 32;

        // 【用户可设置】基础配置
        std::string path;                     ///< 数据源路径/URL（RTSP/HTTP/文件等）
        int buffer_count = 0;                 ///< BufferPool 的 Buffer 数量（0=使用 kDefaultBufferCount）
        int max_frames = -1;                  ///< 最大读取帧数（-1=无限制）
        bool loop = false;                    ///< true=文件播放结束后自动回到开头循环播放
        int raw_frame_width = 0;              ///< 裸帧（YUV）文件的实际宽度（用于 swscale 缩放场景）
        int raw_frame_height = 0;             ///< 裸帧（YUV）文件的实际高度（用于 swscale 缩放场景）
        
        // 【内部参数】框架自动设置，用户不应手动修改
        bool buffer_mode = false;
        const struct AVCodecParameters* codec_params = nullptr;
        AVRational time_base = {0, 1};
        std::shared_ptr<class IEncodedPacketSource> shared_packet_source = nullptr;
        std::shared_ptr<class IRawFrameSource> shared_raw_frame_source = nullptr;
        bool deferred_commit = false;
        
        DataSourceConfig() = default;
        DataSourceConfig(const DataSourceConfig&) = default;
        DataSourceConfig& operator=(const DataSourceConfig&) = default;
        DataSourceConfig(DataSourceConfig&&) = default;
        DataSourceConfig& operator=(DataSourceConfig&&) = default;
    } data_source;
   
    
    // ========================================
    // 解码器配置
    // ========================================
    struct DecoderConfig {
        std::optional<std::string> name;
        bool enable_hardware = true;
        std::optional<std::string> hwaccel_device;
        int decode_threads = 0;

        // Mock 模式（OpenCV 分支新增：生成合成帧，无需实际视频文件）
        bool use_mock = false;             ///< 是否使用 mock 数据源生成合成帧
        int mock_src_width = 0;            ///< mock 帧宽度
        int mock_src_height = 0;           ///< mock 帧高度
        int pix_fmt = -1;                  ///< 像素格式（AV_PIX_FMT_xxx，-1=AV_PIX_FMT_NONE）

        std::unique_ptr<IDecoderVendorExtension> vendor;

        DecoderConfig() = default;
        ~DecoderConfig() = default;

        DecoderConfig(const DecoderConfig& o)
            : name(o.name)
            , enable_hardware(o.enable_hardware)
            , hwaccel_device(o.hwaccel_device)
            , decode_threads(o.decode_threads)
            , use_mock(o.use_mock)
            , mock_src_width(o.mock_src_width)
            , mock_src_height(o.mock_src_height)
            , pix_fmt(o.pix_fmt)
            , vendor(o.vendor ? o.vendor->clone() : nullptr) {}

        DecoderConfig& operator=(const DecoderConfig& o) {
            if (this == &o) return *this;
            name = o.name;
            enable_hardware = o.enable_hardware;
            hwaccel_device = o.hwaccel_device;
            decode_threads = o.decode_threads;
            use_mock = o.use_mock;
            mock_src_width = o.mock_src_width;
            mock_src_height = o.mock_src_height;
            pix_fmt = o.pix_fmt;
            vendor = o.vendor ? o.vendor->clone() : nullptr;
            return *this;
        }

        DecoderConfig(DecoderConfig&&) noexcept = default;
        DecoderConfig& operator=(DecoderConfig&&) noexcept = default;
    } decoder;
    
    // ========================================
    // 编码器配置
    // ========================================
    struct EncoderConfig {
        std::optional<std::string> name;
        bool enable_hardware = true;
        
        int width = 0;                   ///< 编码输入帧宽度（0=从帧源自动获取）
        int height = 0;                  ///< 编码输入帧高度（0=从帧源自动获取）
        
        int64_t bit_rate = 4000000;
        int gop_size = 30;
        int max_b_frames = 0;
        int framerate_num = 30;
        int framerate_den = 1;
        
        int input_pix_fmt = 23;  // AV_PIX_FMT_NV12
        int rc_mode = 1;         // VBR
        int cqp_qp = 0;         ///< CQP 模式下的 QP 值（0=由编码器决定）
        
        struct JpegConfig {
            int quality = 80;
        } jpeg;

        /// 厂商专用编码参数（如 TacoEncoderExtension）
        std::unique_ptr<IEncoderVendorExtension> vendor;

        EncoderConfig() = default;
        ~EncoderConfig() = default;

        EncoderConfig(const EncoderConfig& o)
            : name(o.name)
            , enable_hardware(o.enable_hardware)
            , width(o.width)
            , height(o.height)
            , bit_rate(o.bit_rate)
            , gop_size(o.gop_size)
            , max_b_frames(o.max_b_frames)
            , framerate_num(o.framerate_num)
            , framerate_den(o.framerate_den)
            , input_pix_fmt(o.input_pix_fmt)
            , rc_mode(o.rc_mode)
            , jpeg(o.jpeg)
            , vendor(o.vendor ? o.vendor->clone() : nullptr) {}

        EncoderConfig& operator=(const EncoderConfig& o) {
            if (this == &o) return *this;
            name = o.name;
            enable_hardware = o.enable_hardware;
            width = o.width;
            height = o.height;
            bit_rate = o.bit_rate;
            gop_size = o.gop_size;
            max_b_frames = o.max_b_frames;
            framerate_num = o.framerate_num;
            framerate_den = o.framerate_den;
            input_pix_fmt = o.input_pix_fmt;
            rc_mode = o.rc_mode;
            jpeg = o.jpeg;
            vendor = o.vendor ? o.vendor->clone() : nullptr;
            return *this;
        }

        EncoderConfig(EncoderConfig&&) noexcept = default;
        EncoderConfig& operator=(EncoderConfig&&) noexcept = default;
    } encoder;
    
    // ========================================
    // 全局配置（Worker 类型、线程池等）
    // ========================================
    struct GlobalConfig {
        WorkerType worker_type = WorkerType::AUTO;
        int thread_pool_size = 64;

        GlobalConfig() = default;
        GlobalConfig(const GlobalConfig&) = default;
        GlobalConfig& operator=(const GlobalConfig&) = default;
        GlobalConfig(GlobalConfig&&) = default;
        GlobalConfig& operator=(GlobalConfig&&) = default;
    } global;
    
    // ========================================
    // 消费类型配置
    // ========================================
    using ConsumerTypeConfig = ::ConsumerTypeConfig;
    ConsumerTypeConfig consumer_type;
    
    WorkerConfig() = default;
    WorkerConfig(const WorkerConfig&) = default;
    WorkerConfig& operator=(const WorkerConfig&) = default;
    WorkerConfig(WorkerConfig&&) = default;
    WorkerConfig& operator=(WorkerConfig&&) = default;
};

#endif // WORKER_CONFIGS_HPP
