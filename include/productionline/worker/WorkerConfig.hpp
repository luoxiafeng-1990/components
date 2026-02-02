#ifndef WORKER_CONFIG_HPP
#define WORKER_CONFIG_HPP

#include <string>
#include <string_view>
#include <optional>
#include <memory>
#include <vector>
#include <map>
#include <functional>
#include "common/Logger.hpp"

// FFmpeg 头文件（用于 AVRational 和 AVCodecParameters）
extern "C" {
#include <libavutil/rational.h>
}

// 前向声明（避免循环依赖）
class IEncodedPacketSource;
class Buffer;

// ⭐ v2.23 新增：帧同步回调类型（前向声明）
// 完整定义在 WorkerSyncCoordinator.hpp 中
using FrameSyncCallback = std::function<bool(
    uint64_t frame_version,
    const std::map<std::string, Buffer*>& worker_buffers,
    void* context
)>;

// ⭐ v2.23 新增：回调链项
struct CallbackChainItem {
    FrameSyncCallback callback;
    void* context;
    std::string name;
    
    CallbackChainItem(FrameSyncCallback cb, void* ctx, const std::string& n)
        : callback(cb), context(ctx), name(n) {}
};

// ⭐ v2.23 新增：回调链类型
using CallbackChain = std::vector<CallbackChainItem>;

/**
 * @brief Worker 类型枚举
 * 
 * 注意：此枚举独立定义，避免与 BufferFillingWorkerFactory 的循环依赖
 * 
 * v3.0 重构：
 * - FFMPEG_DECODE: 统一的解码 Worker（支持文件/RTSP/Buffer 模式）
 * - FFMPEG_PACKET_RECORDER: 录制 Worker
 */
enum class WorkerType {
    AUTO,                   // 自动检测（默认）
    FFMPEG_DECODE,          // FFmpeg 解码 Worker（统一处理文件和 RTSP 流）
    FFMPEG_PACKET_RECORDER  // FFmpeg Packet 录制器（支持 RTSP/文件/HTTP 等多种数据源）
};

/**
 * @brief 通道枚举
 * 
 * TACO 解码器支持两个输出通道：
 * - CH0: YUV 格式输出通道
 * - CH1: RGB/YUV 格式输出通道（支持格式转换）
 */
enum class Channel { 
    CH0 = 0,  // 通道0（仅支持 YUV 格式输出）
    CH1 = 1   // 通道1（支持 RGB 和 YUV 格式输出）
};

/**
 * @brief 输出格式枚举
 * 
 * 包含 TACO 解码器支持的所有输出格式（YUV 和 RGB）
 */
enum class OutputFormat {
    // ========================================
    // YUV 格式（通道0和通道1都支持）
    // ========================================
    YUV_AUTO = -1,        // 自动选择 YUV 格式（由解码器根据输入流决定）
    YUV_NV12 = 0,         // NV12: YUV420 semi-planar, UV interleaved
    YUV_NV21 = 1,         // NV21: YUV420 semi-planar, VU interleaved
    YUV_I420 = 2,         // I420/YUV420P: YUV420 planar
    YUV_YV12 = 3,         // YV12: YUV420 planar, V before U
    YUV_P010 = 4,         // P010: 10-bit YUV420 semi-planar
    YUV_NV16 = 5,         // NV16: YUV422 semi-planar
    YUV_NV61 = 6,         // NV61: YUV422 semi-planar, VU interleaved
    YUV_I422 = 7,         // I422: YUV422 planar
    YUV_NV24 = 8,         // NV24: YUV444 semi-planar
    YUV_I444 = 9,         // I444: YUV444 planar
    
    // ========================================
    // RGB 格式（仅通道1支持）
    // ========================================
    // 注意：枚举值 >= 1000 用于与 YUV 格式区分
    RGB_ARGB888 = 1000,      // ARGB8888 packed (驱动值: 9)
    RGB_ABGR888 = 1001,      // ABGR8888 packed (驱动值: 11)
    RGB_RGBA888 = 1002,      // RGBA8888 packed (驱动值: 13)
    RGB_BGRA888 = 1003,      // BGRA8888 packed (驱动值: 15)
    RGB_RGB888 = 1004,       // RGB888 packed (驱动值: 1)
    RGB_BGR888 = 1005,       // BGR888 packed (驱动值: 3)
    RGB_XRGB888 = 1006,      // XRGB8888 packed (驱动值: 25)
    RGB_XBGR888 = 1007,      // XBGR8888 packed (驱动值: 27)
    RGB_RGBX888 = 1008,      // RGBX8888 packed (驱动值: 21)
    RGB_BGRX888 = 1009,      // BGRX8888 packed (驱动值: 23)
    RGB_RGB888_PLANAR = 1010,   // RGB888 planar (驱动值: 2)
    RGB_BGR888_PLANAR = 1011,   // BGR888 planar (驱动值: 4)
    RGB_R16G16B16 = 1012,    // RGB 16-bit per channel (驱动值: 17)
    RGB_B16G16R16 = 1013,    // BGR 16-bit per channel (驱动值: 19)
    RGB_GBRP = 1014          // GBR planar (驱动值: 28)
};

/**
 * @brief 颜色标准枚举
 * 
 * 定义视频颜色空间标准和范围
 */
enum class ColorStandard {
    NONE = 0,               // 无颜色标准
    BT601 = 1,              // BT.601 full range (SD)
    BT601_LIMITED = 2,      // BT.601 limited range
    BT709 = 3,              // BT.709 full range (HD)
    BT709_LIMITED = 4,      // BT.709 limited range
    BT2020 = 5,             // BT.2020 full range (UHD/HDR)
    BT2020_LIMITED = 6      // BT.2020 limited range
};

/**
 * @brief Worker 配置（完整版）
 * 
 * 设计理念：
 * - 完全独立：包含 Worker 需要的所有配置
 * - 配置分离：数据源、显示设备、解码器配置独立
 * - Builder 构建：链式调用，易用易读
 * - 职责清晰：每个 Builder 只负责自己层级的配置
 * 
 * 配置结构：
 * - DataSourceConfig: 数据源路径、模式（Buffer/文件）、共享数据源、编解码器参数
 * - DisplayConfig: 显示设备分辨率和格式
 * - DecoderConfig: 解码器类型和参数
 * - worker_type: Worker 实现类型
 * 
 * ⭐ v2.22 重构：数据源相关配置统一归属 DataSourceConfig
 * - buffer_mode, shared_packet_source, codec_params, time_base 从 DecoderConfig 移至 DataSourceConfig
 * - 逻辑更清晰，职责更明确
 */
struct WorkerConfig {
    // ========================================
    // 数据源配置
    // ========================================
    /**
     * @brief 数据源配置
     * 
     * 用于配置 Worker 的数据源（RTSP 流、HTTP 流、本地文件等）及其相关参数。
     * 
     * ⭐ v2.22 重构：将数据源相关配置从 DecoderConfig 移动到此处
     * - buffer_mode: 数据源模式（Buffer/文件）
     * - shared_packet_source: 共享的 Packet 数据源
     * - codec_params: 编解码器参数（Buffer模式）
     * - time_base: 时间基准（用于同步）
     */
    struct DataSourceConfig {
        // ========================================
        // 基础配置
        // ========================================
        std::string path;                     ///< 数据源路径/URL（RTSP/HTTP/文件等）
        int buffer_count = 0;                 ///< BufferPool 的 Buffer 数量（0=使用 Worker 默认值）
        
        // ========================================
        // 数据源模式配置（v2.22 从 DecoderConfig 移动）
        // ========================================
        bool buffer_mode = false;             ///< true=从Buffer数据源获取packet, false=从文件数据源读取
        const struct AVCodecParameters* codec_params = nullptr;  ///< Buffer模式下的编解码器参数（从Record Worker获取）
        AVRational time_base = {0, 1};        ///< 时间基准（从Record Worker获取，用于同步）
        
        // ⭐ v2.22 共享的 Packet 数据源（从 DecoderConfig 移动）
        // 
        // 使用场景：
        // - 普通模式：nullptr（Worker 自己创建独立的 EncodedPacketSourceFromBuffer）
        // - 共享模式：MultiWorkerProductionLine 创建唯一实例并传入
        // 
        // 优点：
        // - Worker 仍然根据 config 创建 datasource（符合原始设计）
        // - 不需要修改 Worker 接口（不需要 setPacketSource）
        // - 使用基类指针 IEncodedPacketSource，支持多态
        std::shared_ptr<class IEncodedPacketSource> shared_packet_source = nullptr;
        
        DataSourceConfig() = default;
        DataSourceConfig(const DataSourceConfig&) = default;
        DataSourceConfig& operator=(const DataSourceConfig&) = default;
        DataSourceConfig(DataSourceConfig&&) = default;
        DataSourceConfig& operator=(DataSourceConfig&&) = default;
    } data_source;
    
    // ========================================
    // 显示设备配置
    // ========================================
    /**
     * @brief 显示设备配置
     * 
     * 用于配置目标显示设备（如 Framebuffer、显示器）的参数。
     * 
     * ⚠️ 注意：此配置指定的是显示设备分辨率，不是解码器输出分辨率！
     * 解码器输出分辨率请使用 TacoConfig::setDecoderOutputResolution()
     */
    struct DisplayConfig {
        int width = 0;                         ///< 显示设备宽度（像素）
        int height = 0;                        ///< 显示设备高度（像素）
        int bits_per_pixel = 0;                ///< 每像素位数（用于BufferPool内存计算）
        
        DisplayConfig() = default;
    } display;
    
    // ========================================
    // 解码器配置
    // ========================================
    struct DecoderConfig {
        // 通用解码器参数
        std::optional<std::string> name;              // 解码器名称（std::nullopt=自动选择）
        bool enable_hardware = true;                   // 启用硬件加速
        std::optional<std::string> hwaccel_device;     // 硬件设备（如 "cuda:0", "vaapi"）
        int decode_threads = 0;                        // 解码线程数（0=自动）
        
        // ========================================
        // h264_taco 特定配置（子子结构体）
        // ========================================
        struct TacoConfig {
            // ========================================
            // 解码器行为配置
            // ========================================
            bool reorder_disable = true;  // 禁用重排序（推荐保持 true）

            // ========================================
            // 通道0配置（Channel 0 - YUV Output）
            // ========================================
            bool ch0_enable = true;                    // 启用通道0（YUV 格式输出）
            
            // YUV 格式配置
            int ch0_yuv_format = -1;                   // YUV格式类型（-1=自动，0=NV12, 1=NV21, 等）
            int ch0_yuv_std = 1;                       // YUV颜色标准（默认 1=BT.601）
            
            // 裁剪参数（Crop）
            int ch0_crop_x = 0;                        // 裁剪起始X坐标（0=不裁剪）
            int ch0_crop_y = 0;                        // 裁剪起始Y坐标（0=不裁剪）
            int ch0_crop_width = 0;                    // 裁剪宽度（0=不裁剪）
            int ch0_crop_height = 0;                   // 裁剪高度（0=不裁剪）
            
            // 缩放参数（Scale）
            int ch0_scale_width = 0;                   // 缩放目标宽度（0=不缩放）
            int ch0_scale_height = 0;                  // 缩放目标高度（0=不缩放）

            // ========================================
            // 通道1配置（Channel 1 - RGB/YUV Output）
            // ========================================
            bool ch1_enable = false;                   // 启用通道1（默认禁用）
            bool ch1_rgb = false;                      // 是否输出RGB格式（false=YUV）
            
            // RGB 格式配置（仅当 ch1_rgb=true 时有效）
            int ch1_rgb_format = 9;                    // RGB格式类型（默认 9=argb888 packed）
            int ch1_rgb_std = 1;                       // RGB颜色标准（默认 1=BT.601 full range）
            
            // YUV 格式配置（仅当 ch1_rgb=false 时有效）
            int ch1_yuv_format = -1;                   // YUV格式类型（-1=自动）
            int ch1_yuv_std = 1;                       // YUV颜色标准（默认 1=BT.601）
            
            // 裁剪参数（Crop）
            int ch1_crop_x = 0;                        // 裁剪起始X坐标（0=不裁剪）
            int ch1_crop_y = 0;                        // 裁剪起始Y坐标（0=不裁剪）
            int ch1_crop_width = 0;                    // 裁剪宽度（0=不裁剪）
            int ch1_crop_height = 0;                   // 裁剪高度（0=不裁剪）
            
            // 缩放参数（Scale）
            int ch1_scale_width = 0;                   // 缩放目标宽度（0=不缩放）
            int ch1_scale_height = 0;                  // 缩放目标高度（0=不缩放）
            
            TacoConfig() = default;
            TacoConfig(const TacoConfig&) = default;
            TacoConfig& operator=(const TacoConfig&) = default;
            TacoConfig(TacoConfig&&) = default;
            TacoConfig& operator=(TacoConfig&&) = default;
        } taco;
        
        DecoderConfig() = default;
        DecoderConfig(const DecoderConfig&) = default;
        DecoderConfig& operator=(const DecoderConfig&) = default;
        DecoderConfig(DecoderConfig&&) = default;
        DecoderConfig& operator=(DecoderConfig&&) = default;
    } decoder;
    
    // ========================================
    // Worker 类型
    // ========================================
    WorkerType worker_type = WorkerType::AUTO;
    
    // ========================================
    // 消费类型配置（v3.2 重构）
    // ========================================
    /**
     * @brief 消费类型配置
     * 
     * 用于配置 Buffer 消费行为的参数，控制数据的处理、保存和验证。
     * 
     * 设计理念：
     * - 每种消费类型独立配置，通过 enable 标志控制
     * - 支持多种消费类型叠加（如同时显示和保存）
     * - 执行控制参数用于消费循环，与消费类型分离
     * 
     * v3.2 变更：
     * - 结构体名从 ConsumerConfig 改为 ConsumerTypeConfig
     * - 成员变量名从 consumer 改为 consumer_type
     * - 使用嵌套结构体分类不同消费类型
     */
    struct ConsumerTypeConfig {
        // ========================================
        // 执行控制（通用，用于消费循环）
        // ========================================
        int max_frames = -1;              ///< 最大处理帧数（-1=无限制）
        double max_duration_seconds = -1; ///< 最大执行时长（秒，-1=无限制）
        int timeout_ms = 100;             ///< 单次获取 Buffer 超时（毫秒）
        int max_timeout_count = 10;       ///< 最大连续超时次数
        bool verbose = false;             ///< 是否输出详细日志
        
        // ========================================
        // 显示消费类型（CONSUME_DISPLAY）
        // ========================================
        struct DisplayType {
            bool enable = false;          ///< 是否启用显示
            int device_id = 0;            ///< Framebuffer 设备 ID
            DisplayType() = default;
        } display;
        
        // ========================================
        // 保存原始数据消费类型（CONSUME_SAVE_RAW）
        // 用于解码后的 YUV/RGB 数据 → BufferWriter::openRaw()
        // 支持多通道输出：output_paths[0] 对应通道 0，output_paths[1] 对应通道 1，以此类推
        // ========================================
        struct SaveRawType {
            bool enable = false;                      ///< 是否启用保存原始数据
            std::vector<std::string> output_paths;    ///< 输出文件路径列表（按通道顺序）
            std::vector<int> max_frames_per_channel;  ///< 每个通道的最大保存帧数（-1=全部）
            
            SaveRawType() = default;
            
            /// 获取指定通道的输出路径（兼容单路径和多路径）
            std::string getOutputPath(int channel = 0) const {
                if (output_paths.empty()) return "";
                if (channel < 0 || static_cast<size_t>(channel) >= output_paths.size()) {
                    // 如果通道超出范围，使用第一个路径并添加通道后缀
                    if (output_paths.size() == 1 && channel > 0) {
                        const std::string& base = output_paths[0];
                        size_t dot = base.rfind('.');
                        if (dot != std::string::npos) {
                            return base.substr(0, dot) + "_ch" + std::to_string(channel) + base.substr(dot);
                        }
                        return base + "_ch" + std::to_string(channel);
                    }
                    return output_paths[0];
                }
                return output_paths[channel];
            }
            
            /// 设置单个输出路径（兼容旧代码）
            void setOutputPath(const std::string& path) {
                output_paths.clear();
                output_paths.push_back(path);
            }
            
            /// 获取指定通道的最大帧数（兼容单值和多值）
            int getMaxFrames(int channel = 0) const {
                if (max_frames_per_channel.empty()) return -1;
                if (channel < 0 || static_cast<size_t>(channel) >= max_frames_per_channel.size()) {
                    // 如果通道超出范围，使用第一个值
                    return max_frames_per_channel[0];
                }
                return max_frames_per_channel[channel];
            }
            
        } save_raw;
        
        // ========================================
        // 保存编码数据消费类型（CONSUME_SAVE_ENCODED）
        // 用于录制未解码的 H.264/H.265 packet → BufferWriter::openEncoded()
        // ========================================
        struct SaveEncodedType {
            bool enable = false;          ///< 是否启用保存编码数据
            std::string output_path;      ///< 输出文件路径（如 output.mp4）
            SaveEncodedType() = default;
        } save_encoded;
        
        // ========================================
        // 比较配置（用于 COMPARE 执行模式）
        // 注：compare 是执行模式，不是消费类型
        // ========================================
        struct CompareType {
            // ========== 指标开关 ==========
            bool enable_psnr = false;               ///< 是否启用 PSNR 计算（需显式开启）
            bool enable_ssim = false;               ///< 是否启用 SSIM 计算（需显式开启，计算量约为 PSNR 的 1.5-2 倍）
            
            // ========== 验证策略 ==========
            enum Strategy {
                FAST_ONLY,          ///< 仅快速验证（每帧 PSNR-Y/G）
                AUTO_LAYERED,       ///< 自动分层（推荐）
                DEEP_ALWAYS         ///< 总是深度验证（慢但详细）
            };
            Strategy strategy = AUTO_LAYERED;
            
            // ========== 格式处理策略 ==========
            enum FormatStrategy {
                AUTO,               ///< 自动检测并选择最优策略（推荐）
                FORCE_YUV,          ///< 强制转换到 YUV 空间对比
                FORCE_RGB,          ///< 强制转换到 RGB 空间对比
                NATIVE              ///< 原生格式对比（要求两边格式一致）
            };
            FormatStrategy format_strategy = AUTO;
            
            // ========== 色彩空间转换配置 ==========
            enum ColorStandard {
                BT601,              ///< 标清：Rec.601（默认）
                BT709,              ///< 高清：Rec.709
                BT2020              ///< 4K/HDR：Rec.2020
            };
            ColorStandard color_std = BT601;
            
            // ========== 阈值配置 ==========
            double min_psnr = 38.0;                 ///< PSNR 通过阈值（>= 此值快速通过）
            double warn_psnr = 35.0;                ///< PSNR 警告阈值（< 此值触发深度验证）
            double min_ssim = 0.95;                 ///< SSIM 通过阈值（>= 此值认为质量优秀）
            double warn_ssim = 0.90;                ///< SSIM 警告阈值（< 此值触发警告）
            int max_pixel_diff = 3;                 ///< 最大像素差值（灰度级）
            float diff_pixel_ratio = 0.05f;         ///< 差异像素比例阈值（< 5%）
            
            // ========== 并行计算配置 ==========
            bool enable_parallel = true;            ///< 是否启用并行计算（使用全局线程池）
            
            // ========== 感知加权 ==========
            bool use_perceptual_weighting = true;   ///< 是否使用感知加权（YUV:Y权重高，RGB:G权重高）
            
            // ========== 输出选项 ==========
            bool verbose = false;                   ///< 是否输出详细日志
            bool save_report = false;               ///< 是否保存报告到文件
            std::string report_path = "./decoder_compare_report.txt";  ///< 报告文件路径
            bool save_failed_frames = false;        ///< 是否保存失败帧的差异图
            std::string output_dir = "./validation_output";  ///< 输出目录
            
            // ========== ⭐ v2.27 新增：通道比较配置 ==========
            bool enable_channel_compare = false;    ///< 是否启用通道间比较（作为消费类型）
            int reference_channel = 0;              ///< 参考通道号
            int compare_channel = 1;                ///< 比较通道号
            
            CompareType() = default;
        } compare;
        
        // ========================================
        // 性能验证参数
        // ========================================
        struct PerformanceType {
            bool enable = false;          ///< 是否启用性能验证
            double target_fps = 30.0;     ///< 目标帧率
            PerformanceType() = default;
        } performance;
        
        // ========================================
        // 仅统计消费类型（CONSUME_COUNT）
        // ========================================
        struct CountType {
            bool enable = false;          ///< 是否启用统计
            CountType() = default;
        } count;
        
        ConsumerTypeConfig() = default;
        ConsumerTypeConfig(const ConsumerTypeConfig&) = default;
        ConsumerTypeConfig& operator=(const ConsumerTypeConfig&) = default;
        ConsumerTypeConfig(ConsumerTypeConfig&&) = default;
        ConsumerTypeConfig& operator=(ConsumerTypeConfig&&) = default;
    } consumer_type;
    
    // ========================================
    // 全局资源配置
    // ========================================
    /**
     * @brief 全局线程池大小（默认 64，范围：1-128）
     * 
     * 注意：只在第一次调用时生效，如果线程池已初始化则忽略
     * 0 表示不初始化（使用默认值 64）
     */
    int thread_pool_size = 64;
    
    WorkerConfig() = default;
    WorkerConfig(const WorkerConfig&) = default;
    WorkerConfig& operator=(const WorkerConfig&) = default;
    WorkerConfig(WorkerConfig&&) = default;
    WorkerConfig& operator=(WorkerConfig&&) = default;
};

// ========================================
// Builder 模式实现
// ========================================

/**
 * @brief 数据源配置构建器
 */
class DataSourceConfigBuilder {
public:
    DataSourceConfigBuilder() = default;
    
    /**
     * @brief 设置数据源路径/URL
     * @param path 数据源路径（支持 RTSP、HTTP、本地文件等）
     * 
     * @example
     * - RTSP 流：`rtsp://192.168.1.100/stream`
     * - HTTP/HLS 流：`http://example.com/playlist.m3u8`
     * - 本地文件：`/data/video.mp4`
     */
    DataSourceConfigBuilder& setPath(std::string_view path);
    
    // 兼容 const char*（保持向后兼容）
    DataSourceConfigBuilder& setPath(const char* path);
    
    // 兼容 std::string
    DataSourceConfigBuilder& setPath(const std::string& path);
    
    /**
     * @brief 设置 BufferPool 的 Buffer 数量
     * @param count Buffer 数量（0=使用 Worker 默认值）
     * 
     * 建议值：
     * - RTSP 流解码：4-8
     * - 本地文件解码：128
     * - Packet 录制：64
     */
    DataSourceConfigBuilder& setBufferCount(int count);
    
    WorkerConfig::DataSourceConfig build() const;
    
private:
    WorkerConfig::DataSourceConfig data_source_config_;
};

/**
 * @brief 显示设备配置构建器
 */
class DisplayConfigBuilder {
public:
    DisplayConfigBuilder() = default;
    
    /**
     * @brief 设置显示设备宽度
     * @param width 显示设备宽度（像素）
     */
    DisplayConfigBuilder& setDisplayWidth(int width);
    
    /**
     * @brief 设置显示设备高度
     * @param height 显示设备高度（像素）
     */
    DisplayConfigBuilder& setDisplayHeight(int height);
    
    /**
     * @brief 设置显示设备分辨率
     * 
     * @param width  显示设备宽度（像素）
     * @param height 显示设备高度（像素）
     * @return DisplayConfigBuilder& 链式调用
     * 
     * @example
     * ```cpp
     * DisplayConfigBuilder()
     *     .setDisplayResolution(1920, 1080)  // Framebuffer 分辨率
     *     .setBitsPerPixel(32)
     *     .build()
     * ```
     */
    DisplayConfigBuilder& setDisplayResolution(int width, int height);
    
    /**
     * @brief 设置每像素位数
     * @param bpp 每像素位数（如 32 表示 ARGB8888）
     */
    DisplayConfigBuilder& setBitsPerPixel(int bpp);
    
    WorkerConfig::DisplayConfig build() const;
    
private:
    WorkerConfig::DisplayConfig display_config_;
};

/**
 * @brief TACO 解码器特定配置构建器
 * 
 * 提供统一的通用接口来配置 TACO 解码器的两个输出通道。
 * 
 * 设计理念：
 * - 通用接口：所有通道使用相同的配置接口（setCrop/setScale/setOutputFormat）
 * - 类型安全：使用枚举类型代替魔法数字
 * - 无前置检查：可以先配置参数，后启用通道（Builder 模式的正常用法）
 * 
 * 使用示例：
 * @code
 * // 通道0: YUV NV12 输出
 * auto taco = TacoConfigBuilder()
 *     .setChannels(true, false)
 *     .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12, ColorStandard::BT709)
 *     .setScale(Channel::CH0, 1920, 1080)
 *     .build();
 * 
 * // 通道1: RGB BGRA888 输出
 * auto taco = TacoConfigBuilder()
 *     .setChannels(false, true)
 *     .setOutputFormat(Channel::CH1, OutputFormat::RGB_BGRA888, ColorStandard::BT709)
 *     .setCrop(Channel::CH1, 0, 0, 1920, 1080)
 *     .setScale(Channel::CH1, 1280, 720)
 *     .build();
 * @endcode
 */
class TacoConfigBuilder {
public:
    TacoConfigBuilder() = default;
    
    // ========================================
    // 解码器行为配置
    // ========================================
    
    /**
     * @brief 设置是否禁用重排序
     * @param disable true=禁用重排序（推荐），false=启用重排序
     */
    TacoConfigBuilder& setReorderDisable(bool disable = true);
    
    /**
     * @brief 同时设置两个通道的启用状态（快捷方法）
     * @param ch0 是否启用通道0
     * @param ch1 是否启用通道1
     */
    TacoConfigBuilder& setChannels(bool ch0, bool ch1);
    
    // ========================================
    // 通用配置接口（支持任意通道）
    // ========================================
    
    /**
     * @brief 设置通道输出格式（通用接口）
     * 
     * @param ch 通道选择（CH0 或 CH1）
     * @param format 输出格式
     *               - YUV_*: YUV 格式（两个通道都支持）
     *               - RGB_*: RGB 格式（仅 CH1 支持）
     * @param std 颜色标准（默认 BT601）
     * 
     * @note 通道0仅支持 YUV 格式，传入 RGB 格式会记录错误并忽略
     * @note 通道1支持 RGB 和 YUV 格式
     * @note 自动根据 format 判断是 RGB 还是 YUV 并设置对应参数
     * 
     * @example
     * // 通道0设置 YUV NV12
     * .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12, ColorStandard::BT601)
     * 
     * // 通道1设置 RGB ARGB888
     * .setOutputFormat(Channel::CH1, OutputFormat::RGB_ARGB888, ColorStandard::BT709)
     * 
     * // 通道1设置 YUV P010 (10-bit)
     * .setOutputFormat(Channel::CH1, OutputFormat::YUV_P010, ColorStandard::BT2020)
     */
    TacoConfigBuilder& setOutputFormat(
        Channel ch,
        OutputFormat format = OutputFormat::YUV_AUTO,
        ColorStandard std = ColorStandard::BT601
    );
    
    /**
     * @brief 设置通道裁剪区域（通用接口）
     * 
     * @param ch 通道选择（CH0 或 CH1）
     * @param x 裁剪起始 X 坐标（0=不裁剪）
     * @param y 裁剪起始 Y 坐标（0=不裁剪）
     * @param width 裁剪宽度（0=不裁剪）
     * @param height 裁剪高度（0=不裁剪）
     * 
     * @note 无需预先启用通道，可以先配置后启用
     * 
     * @example
     * // 通道0裁剪
     * .setCrop(Channel::CH0, 100, 100, 1920, 1080)
     * 
     * // 通道1裁剪
     * .setCrop(Channel::CH1, 0, 0, 1280, 720)
     */
    TacoConfigBuilder& setCrop(Channel ch, int x, int y, int width, int height);
    
    /**
     * @brief 设置通道缩放分辨率（通用接口）
     * 
     * @param ch 通道选择（CH0 或 CH1）
     * @param width 缩放目标宽度（0=不缩放）
     * @param height 缩放目标高度（0=不缩放）
     * 
     * @note 无需预先启用通道，可以先配置后启用
     * 
     * @example
     * // 通道0缩放到 1920x1080
     * .setScale(Channel::CH0, 1920, 1080)
     * 
     * // 通道1缩放到 1280x720
     * .setScale(Channel::CH1, 1280, 720)
     */
    TacoConfigBuilder& setScale(Channel ch, int width, int height);
    
    /**
     * @brief 构建最终的 TacoConfig 对象
     */
    WorkerConfig::DecoderConfig::TacoConfig build() const;
    
    // ========================================
    // 辅助映射函数（向后兼容，供外部使用）
    // ========================================
    
    /**
     * @brief 将格式名称字符串映射为 OutputFormat 枚举
     * @param format_name 格式名称（如 "nv12", "argb888" 等）
     * @return OutputFormat 枚举值
     * 
     * 向后兼容旧代码使用字符串配置的情况。
     */
    static OutputFormat mapFormatNameToEnum(std::string_view format_name);
    
    /**
     * @brief 将颜色标准名称字符串映射为 ColorStandard 枚举
     * @param std_name 颜色标准名称（如 "bt601", "bt709" 等）
     * @return ColorStandard 枚举值
     * 
     * 向后兼容旧代码使用字符串配置的情况。
     */
    static ColorStandard mapColorStdNameToEnum(std::string_view std_name);
    
    /**
     * @brief 将 OutputFormat 枚举映射为格式名称字符串
     * @param format OutputFormat 枚举值
     * @return 格式名称字符串
     */
    static std::string_view mapFormatEnumToName(OutputFormat format);
    
    /**
     * @brief 将 ColorStandard 枚举映射为颜色标准名称字符串
     * @param std ColorStandard 枚举值
     * @return 颜色标准名称字符串
     */
    static std::string_view mapColorStdEnumToName(ColorStandard std);

private:
    WorkerConfig::DecoderConfig::TacoConfig taco_config_;
    
    /**
     * @brief 将 OutputFormat 枚举值映射回 TACO 驱动的原始 RGB 格式值
     * 
     * OutputFormat 枚举使用 1000+ 的值来区分 RGB 和 YUV，
     * 但 TACO 驱动需要原始的格式值（如 9 表示 ARGB888）。
     */
    static int mapEnumToRgbDriverValue(OutputFormat format);
};

/**
 * @brief 解码器配置构建器
 */
class DecoderConfigBuilder {
public:
    DecoderConfigBuilder() = default;
    
    // ========== 通用解码器参数 ==========
    
    // 接受 std::string_view（推荐）
    DecoderConfigBuilder& setDecoderName(std::string_view name);
    
    // 兼容 const char*（保持向后兼容）
    DecoderConfigBuilder& setDecoderName(const char* name);
    
    // 清除解码器名称（使用自动选择）
    DecoderConfigBuilder& clearDecoderName();
    
    // 接受 std::string_view（推荐）
    DecoderConfigBuilder& setHwaccelDevice(std::string_view device);
    
    // 兼容 const char*（保持向后兼容）
    DecoderConfigBuilder& setHwaccelDevice(const char* device);
    
    DecoderConfigBuilder& setDecodeThreads(int threads);
    
    // ========== 快捷预设 ==========
    /**
     * @brief 预设：TACO 硬件解码（通用，支持自定义配置）
     * 
     * 设置解码器为 TACO 平台的指定编解码器，并使用自定义 TACO 配置。
     * 
     * @param codec 编解码器类型（如 "h264"、"h265"、"vp9" 等）
     * @param taco_config 自定义的 TACO 配置对象
     * 
     * 示例：
     * @code
     * auto tacoConfig = TacoConfigBuilder()
     *     .setRgbConfig(true, "bgra888", "bt709")
     *     .setDecoderOutputResolution(1920, 1080)
     *     .build();
     * 
     * DecoderConfigBuilder().useTaco("h264", tacoConfig).build()
     * @endcode
     */
    DecoderConfigBuilder& useTaco(
        std::string_view codec,
        const WorkerConfig::DecoderConfig::TacoConfig& taco_config
    );
    
    /**
     * @brief 预设：软件解码（自动选择）
     */
    DecoderConfigBuilder& useSoftware();
    
    /**
     * @brief 预设：NVIDIA CUDA 硬件解码（通用，支持多种编解码器）
     * 
     * 设置解码器为 NVIDIA CUDA 平台的指定编解码器。
     * CUDA 是 NVIDIA GPU 硬件加速平台，支持 H.264、H.265、VP9、AV1 等。
     * 
     * @param codec 编解码器类型（如 "h264"、"h265"、"vp9"、"av1" 等）
     * 
     * 示例：
     * @code
     * // H.264 CUDA 解码
     * DecoderConfigBuilder().useCuvid("h264").build()
     * 
     * // H.265/HEVC CUDA 解码
     * DecoderConfigBuilder().useCuvid("h265").build()
     * 
     * // VP9 CUDA 解码
     * DecoderConfigBuilder().useCuvid("vp9").build()
     * 
     * // AV1 CUDA 解码
     * DecoderConfigBuilder().useCuvid("av1").build()
     * @endcode
     * 
     * 生成的解码器名称格式为：{codec}_cuvid（如 "h264_cuvid"、"h265_cuvid"）
     */
    DecoderConfigBuilder& useCuvid(std::string_view codec);
    
    /**
     * @brief 预设：Intel Quick Sync Video 硬件解码（通用，支持多种编解码器）
     * 
     * 设置解码器为 Intel QSV 平台的指定编解码器。
     * QSV 是 Intel 集成显卡硬件加速平台，支持 H.264、H.265、VP9、AV1 等。
     * 
     * @param codec 编解码器类型（如 "h264"、"h265"、"vp9"、"av1" 等）
     * 
     * 示例：
     * @code
     * // H.264 QSV 解码
     * DecoderConfigBuilder().useQsv("h264").build()
     * 
     * // H.265/HEVC QSV 解码
     * DecoderConfigBuilder().useQsv("h265").build()
     * 
     * // VP9 QSV 解码
     * DecoderConfigBuilder().useQsv("vp9").build()
     * 
     * // AV1 QSV 解码
     * DecoderConfigBuilder().useQsv("av1").build()
     * @endcode
     * 
     * 生成的解码器名称格式为：{codec}_qsv（如 "h264_qsv"、"h265_qsv"）
     */
    DecoderConfigBuilder& useQsv(std::string_view codec);
    
    /**
     * @brief 预设：VA-API 硬件解码（通用，支持多种编解码器）
     * 
     * 设置解码器为 VA-API 平台的指定编解码器。
     * VA-API 是 Linux 视频加速 API，支持 Intel/AMD GPU 硬件加速。
     * 
     * @param codec 编解码器类型（如 "h264"、"h265"、"vp9"、"av1" 等）
     * 
     * 示例：
     * @code
     * // H.264 VA-API 解码
     * DecoderConfigBuilder().useVaapi("h264").build()
     * 
     * // H.265/HEVC VA-API 解码
     * DecoderConfigBuilder().useVaapi("h265").build()
     * 
     * // VP9 VA-API 解码
     * DecoderConfigBuilder().useVaapi("vp9").build()
     * @endcode
     * 
     * 生成的解码器名称格式为：{codec}_vaapi（如 "h264_vaapi"、"h265_vaapi"）
     */
    DecoderConfigBuilder& useVaapi(std::string_view codec);
    
    WorkerConfig::DecoderConfig build() const;
    
private:
    WorkerConfig::DecoderConfig decoder_config_;
};

/**
 * @brief Worker 配置构建器（顶层）
 * 
 * 职责：只负责组装 WorkerConfig，不涉及具体配置细节
 */
class WorkerConfigBuilder {
public:
    WorkerConfigBuilder() = default;
    
    /**
     * @brief 设置数据源配置
     */
    WorkerConfigBuilder& setDataSourceConfig(const WorkerConfig::DataSourceConfig& data_source_config);
    
    /**
     * @brief 设置显示设备配置
     * @param display_config 显示设备配置
     */
    WorkerConfigBuilder& setDisplayConfig(const WorkerConfig::DisplayConfig& display_config);
    
    /**
     * @brief 设置解码器配置
     */
    WorkerConfigBuilder& setDecoderConfig(const WorkerConfig::DecoderConfig& decoder_config);
    
    /**
     * @brief 设置 Worker 类型
     */
    WorkerConfigBuilder& setWorkerType(WorkerType type);
    
    /**
     * @brief 设置全局线程池大小
     * 
     * @param size 线程池大小（范围：1-128，默认：64）
     * 
     * @note 验证规则（在 VideoProductionLine::start() 中执行）：
     *   - 必须 > 0，否则使用默认值 64
     *   - 最大 128，超过则使用 128
     *   - 0 表示使用默认值 64
     * 
     * @note 注意：只在第一次调用时生效，如果线程池已初始化则忽略
     * 
     * @example
     * ```cpp
     * WorkerConfigBuilder()
     *     .setThreadPoolSize(32)
     *     .setDataSourceConfig(...)
     *     .build()
     * ```
     */
    WorkerConfigBuilder& setThreadPoolSize(int size);
    
    // ========================================
    // 消费类型配置（v3.2 重构）
    // ========================================
    
    /**
     * @brief 设置消费类型配置
     */
    WorkerConfigBuilder& setConsumerTypeConfig(const WorkerConfig::ConsumerTypeConfig& consumer_type_config);
    
    /**
     * @brief 设置最大处理帧数（执行控制）
     * @param frames 最大帧数（-1=无限制）
     */
    WorkerConfigBuilder& setMaxFrames(int frames);
    
    /**
     * @brief 启用显示消费类型
     * @param enable 是否启用
     * @param device_id Framebuffer 设备 ID
     */
    WorkerConfigBuilder& enableDisplay(bool enable = true, int device_id = 0);
    
    /**
     * @brief 启用保存原始数据消费类型
     * @param enable 是否启用
     * @param output_path 输出文件路径
     * @param max_frames 最大保存帧数（-1=全部）
     */
    WorkerConfigBuilder& enableSaveRaw(bool enable = true, 
                                        const std::string& output_path = "",
                                        int max_frames = -1);
    
    /**
     * @brief 启用保存编码数据消费类型
     * @param enable 是否启用
     * @param output_path 输出文件路径
     */
    WorkerConfigBuilder& enableSaveEncoded(bool enable = true,
                                            const std::string& output_path = "");
    
    /**
     * @brief 启用比较消费类型
     * @param enable 是否启用
     * @param min_psnr PSNR 阈值（dB）
     * @param min_ssim SSIM 阈值（0.0-1.0）
     */
    WorkerConfigBuilder& enableCompare(bool enable = true, 
                                        double min_psnr = 30.0, 
                                        double min_ssim = 0.95);
    
    /**
     * @brief 启用性能验证
     * @param enable 是否启用
     * @param target_fps 目标帧率
     */
    WorkerConfigBuilder& enablePerformance(bool enable = true, double target_fps = 30.0);
    
    /**
     * @brief 设置详细日志模式
     */
    WorkerConfigBuilder& setVerbose(bool verbose);
    
    /**
     * @brief 构建最终配置
     */
    WorkerConfig build() const;
    
private:
    WorkerConfig worker_config_;
};

// ============================================================
// MultiWorker 配置结构（v2.20 新增：从 MultiWorkerProductionLine 移动）
// ============================================================

/**
 * @brief ProducerConfig - 生产者配置
 */
struct ProducerConfig {
    std::string producer_name;      // 组内唯一标识
    WorkerConfig worker_config;
};

/**
 * @brief ConsumerConfig - 消费者配置
 */
struct ConsumerConfig {
    std::string consumer_name;      // 组内唯一标识（可选）
    WorkerConfig worker_config;
};

/**
 * @brief Connector - 连接器类
 * 
 * 核心职责：
 * - 定义生产者-消费者之间的映射规则（1:1, 1:N, N:1, N:M）
 * - 为每个消费者分配应该绑定的生产者 BufferPool
 * - 不直接处理数据，只提供路由配置
 * 
 * 设计原则：
 * - 简单：单一类，通过 Mode 枚举选择模式
 * - 必要字段：mode, producer_names, consumer_names
 * - 核心方法：getProducerNameForConsumer()
 * 
 * ⭐ v2.18 新增：
 * - 支持共享 PacketSource（ONE_TO_MANY 模式）
 * - 存储共享实例，防止被销毁
 * 
 * ⭐ v2.20：从 Connector.hpp 移动到 WorkerConfig.hpp（统一配置管理）
 * 
 * ⭐ v2.21：重构为使用名字而非索引，提高一致性和可读性
 */
class Connector {
public:
    enum class Mode {
        ONE_TO_ONE,      // 1:1 映射
        ONE_TO_MANY,     // 1:N 映射（广播模式）
        MANY_TO_ONE,     // N:1 映射（合并模式）
        MANY_TO_MANY     // N:M 映射（轮询策略）
    };
    
    /**
     * @brief 构造函数
     * @param mode 连接器模式
     * @param producer_names 生产者名称列表
     * @param consumer_names 消费者名称列表
     */
    Connector(Mode mode,
              const std::vector<std::string>& producer_names,
              const std::vector<std::string>& consumer_names);
    
    /**
     * @brief 获取消费者对应的生产者名称
     * @param consumer_name 消费者名称
     * @return 生产者名称，空字符串表示没有对应的生产者
     */
    std::string getProducerNameForConsumer(const std::string& consumer_name) const;
    
    /**
     * @brief 检查是否包含指定生产者
     * @param producer_name 生产者名称
     * @return true 如果包含该生产者，false 否则
     */
    bool containsProducer(const std::string& producer_name) const;
    
    /**
     * @brief 检查是否包含指定消费者
     * @param consumer_name 消费者名称
     * @return true 如果包含该消费者，false 否则
     */
    bool containsConsumer(const std::string& consumer_name) const;
    
    // 访问器
    Mode getMode() const;
    const std::vector<std::string>& getProducerNames() const;
    const std::vector<std::string>& getConsumerNames() const;
    
    // ⭐ v2.18 新增：设置共享的 EncodedPacketSource（按生产者名称）
    /**
     * @brief 为指定生产者设置共享的 EncodedPacketSource
     * @param producer_name 生产者名称
     * @param source 共享的 EncodedPacketSource 实例
     * 
     * 功能：
     * - Connector 持有共享实例，防止被销毁
     * - 每个生产者都有自己独立的共享数据源
     * - 支持多个生产者，每个生产者对应一个共享数据源
     */
    void setSharedSource(const std::string& producer_name, std::shared_ptr<class IEncodedPacketSource> source);
    
    /**
     * @brief 获取指定生产者的共享 EncodedPacketSource
     * @param producer_name 生产者名称
     * @return 共享实例（如果没有则返回 nullptr）
     */
    std::shared_ptr<class IEncodedPacketSource> getSharedSource(const std::string& producer_name) const;

private:
    Mode mode_;
    std::vector<std::string> producer_names_;
    std::vector<std::string> consumer_names_;
    
    // ⭐ v2.18 新增：共享的 EncodedPacketSource（按生产者名称索引）
    // 每个生产者都有自己独立的共享数据源
    std::map<std::string, std::shared_ptr<class IEncodedPacketSource>> shared_sources_;
};

/**
 * @brief ConnectorConfig - 连接器配置
 */
struct ConnectorConfig {
    Connector::Mode mode;
    std::vector<std::string> producer_names;  // 关联的生产者名称
    std::vector<std::string> consumer_names;   // 关联的消费者名称
    
    // ⭐ v2.23 新增：帧同步配置
    bool enable_frame_sync = false;          // 是否启用帧同步
    CallbackChain callback_chain;            // 回调链（可选）
};

/**
 * @brief WorkerGroupConfig - Worker 工作组配置结构
 * 
 * ⭐ 设计说明：配置与运行时分离模式（Configuration vs Runtime State）
 * 
 * 职责：
 * - 描述"要创建什么"（配置数据）
 * - 在构造函数时传入，整个生命周期只读
 * - 可序列化/反序列化（纯数据，不包含对象实例）
 * 
 * ⭐ 核心概念：一个 Group = 多个生产者 + 多个消费者 + 多个连接器
 * - Group 内强同步：通过连接器建立生产者-消费者关系
 * - Group 间独立：多个 Group 并行运行，互不干扰
 * - 数据源模式：消费者自动配置为 Buffer 模式，关联到生产者的 BufferPool
 * 
 * 注意：运行时数据（实际创建的对象、线程、统计信息等）存储在 WorkerGroupRuntime 中
 * 
 * @see WorkerGroupRuntime - 对应的运行时数据结构
 */
struct WorkerGroupConfig {
    // 组标识
    std::string group_id;
    
    // 多个生产者和消费者配置
    std::vector<ProducerConfig> producer_configs;
    std::vector<ConsumerConfig> consumer_configs;
    
    // 多个连接器配置
    std::vector<ConnectorConfig> connector_configs;
    
    WorkerGroupConfig() = default;
    explicit WorkerGroupConfig(const std::string& id) : group_id(id) {}
};

// 向后兼容：保留 WorkerGroup 作为 WorkerGroupConfig 的别名
using WorkerGroup = WorkerGroupConfig;

/**
 * @brief MultiWorkerConfig - 多Worker配置结构
 * 
 * 设计理念：
 * - 包含全局配置（如线程池大小）
 * - 包含多个 WorkerGroup，每个 Group 包含多个生产者和消费者
 * - 支持复杂的多 Worker 协作场景
 */
struct MultiWorkerConfig {
    // ⭐ 核心：Worker Group 配置列表
    std::vector<WorkerGroupConfig> groups;
    
    // 全局线程池配置（用于初始化全局线程池）
    // 默认值：64，范围：1-128
    int thread_pool_size = 64;
    
    MultiWorkerConfig() = default;
};

#endif // WORKER_CONFIG_HPP



