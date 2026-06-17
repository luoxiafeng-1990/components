#ifndef CONSUMPTIONLINE_CONFIG_CONSUMER_TYPE_CONFIG_HPP
#define CONSUMPTIONLINE_CONFIG_CONSUMER_TYPE_CONFIG_HPP

#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <functional>
#include <cstdint>
#include "vendor/contracts/DisplayVendorExtension.hpp"
#include "vendor/contracts/NpuInferenceVendorExtension.hpp"
#include "vendor/contracts/OpencvVendorExtension.hpp"

/**
 * @brief 消费类型配置（整包：执行控制 + 多种可叠加的消费能力）
 *
 * 描述**消费线程/策略**在拿到解码后的 Buffer 时要做什么：是否上屏、是否落盘、是否
 * NPU 推理、是否做画质对比等。`WorkerConfigBuilder::setConsumerTypeConfig` 一次赋值
 * **整个**本结构体，不是只配置某一种消费类型。
 *
 * 成员一览（与常见 CONSUME_* 标志对应关系见各子结构注释）：
 * | 成员 | 含义 |
 * |------|------|
 * | （本段标量） | 消费循环：`max_frames`、`timeout_ms`、`verbose` 等 |
 * | `display` | CONSUME_DISPLAY：是否走显示消费、device_id、vendor 厂商扩展 |
 * | `save_raw` | CONSUME_SAVE_RAW：解码后 YUV/RGB 写文件 |
 * | `save_encoded` | CONSUME_SAVE_ENCODED：未解码 packet 写文件 |
 * | `npu_inference` | CONSUME_NPU_INFERENCE：NPU 模型推理 |
 * | `compare` | 画质/通道比较（常与 COMPARE 执行模式配合） |
 * | `performance` | 性能统计（目标 FPS 等） |
 * | `count` | CONSUME_COUNT：仅统计帧数 |
 *
 * 设计理念：
 * - 各消费子块独立，各自 `enable`（或等价开关），可多选同时开启
 * - 执行控制字段作用于整段消费循环，与具体「消费种类」正交
 *
 * v3.2：`ConsumerConfig` 更名为 `ConsumerTypeConfig`，成员名为 `consumer_type`。
 */
struct ConsumerTypeConfig {
    // ========================================
    // 执行控制（通用，用于消费循环；不属于某一类 CONSUME_*）
    // ========================================
    int max_frames = -1;              ///< 最大处理帧数（-1=无限制）
    double max_duration_seconds = -1; ///< 最大执行时长（秒，-1=无限制）
    int timeout_ms = 100;             ///< 单次获取 Buffer 超时（毫秒）
    int max_timeout_count = 10;       ///< 最大连续超时次数
    bool verbose = false;             ///< 是否输出详细日志
    
    // ========================================
    // 显示消费类型（CONSUME_DISPLAY）
    // ========================================
    /**
     * @brief 显示消费配置
     *
     * 通用壳（enable / device_id）+ 厂商扩展指针 vendor。
     * 具体厂商参数在 TacoProDisplayExtension / TacoDisplayExtension 中。
     * 拷贝时通过 vendor->clone() 深拷贝。
     */
    struct DisplayConsumerConfig {
        bool enable = false;          ///< 是否启用显示
        int device_id = 0;            ///< 显示设备 ID

        /// 厂商专有参数；nullptr 表示未选择厂商
        std::unique_ptr<IDisplayVendorExtension> vendor;

        DisplayConsumerConfig() = default;
        ~DisplayConsumerConfig() = default;

        DisplayConsumerConfig(const DisplayConsumerConfig& o)
            : enable(o.enable)
            , device_id(o.device_id)
            , vendor(o.vendor ? o.vendor->clone() : nullptr) {}

        DisplayConsumerConfig& operator=(const DisplayConsumerConfig& o) {
            if (this == &o) return *this;
            enable = o.enable;
            device_id = o.device_id;
            vendor = o.vendor ? o.vendor->clone() : nullptr;
            return *this;
        }

        DisplayConsumerConfig(DisplayConsumerConfig&&) noexcept = default;
        DisplayConsumerConfig& operator=(DisplayConsumerConfig&&) noexcept = default;
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
    // NPU 推理消费类型（CONSUME_NPU_INFERENCE）⭐ v2.28 新增
    // 将解码帧送入 NPU 进行模型推理（如目标检测）
    // ========================================
    struct NpuInferenceType {
        bool enable = false;                  ///< 是否启用 NPU 推理
        std::string model_path;               ///< .nb 模型文件路径
        float conf_threshold  = 0.25f;        ///< 置信度阈值
        float nms_threshold   = 0.45f;        ///< NMS IoU 阈值
        int   npu_core_index  = 0;            ///< NPU 核心索引
        bool  use_physical_addr = false;      ///< 是否使用物理地址输入（零拷贝，需模型支持 NV12）
        bool  enable_draw = false;            ///< 推理后在 buffer 上画检测框（供 Display 显示带框画面）
        int   inference_interval = 1;         ///< 每 N 帧执行一次推理（1=每帧，>1 跳帧以保证显示流畅）

        std::unique_ptr<INpuInferenceVendorExtension> vendor;

        NpuInferenceType() = default;
        NpuInferenceType(const NpuInferenceType& o)
            : enable(o.enable), model_path(o.model_path)
            , conf_threshold(o.conf_threshold), nms_threshold(o.nms_threshold)
            , npu_core_index(o.npu_core_index), use_physical_addr(o.use_physical_addr)
            , enable_draw(o.enable_draw), inference_interval(o.inference_interval)
            , vendor(o.vendor ? o.vendor->clone() : nullptr) {}
        NpuInferenceType& operator=(const NpuInferenceType& o) {
            if (this != &o) {
                enable = o.enable; model_path = o.model_path;
                conf_threshold = o.conf_threshold; nms_threshold = o.nms_threshold;
                npu_core_index = o.npu_core_index; use_physical_addr = o.use_physical_addr;
                enable_draw = o.enable_draw; inference_interval = o.inference_interval;
                vendor = o.vendor ? o.vendor->clone() : nullptr;
            }
            return *this;
        }
        NpuInferenceType(NpuInferenceType&&) = default;
        NpuInferenceType& operator=(NpuInferenceType&&) = default;
    } npu_inference;
    
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

    // ========================================
    // JPEG 编码消费类型（CONSUME_JPEG_ENCODE）v3.3 新增
    // 将解码帧编码为 JPEG，通过命名管道输出（WebUI 预览）
    // ========================================
    struct JpegEncodeType {
        bool enable = false;                          ///< 是否启用 JPEG 编码
        std::string output_pipe;                      ///< FIFO 路径（空=不使用 FIFO）
        int quality = 80;                             ///< JPEG 质量 1-100
        int target_fps = 15;                          ///< 目标帧率（降帧）
        std::string encoder_name = "jpeg_taco";       ///< 编码器名（jpeg_taco / mjpeg）

        /// 内存回调模式：同进程内直接接收 JPEG 帧（webui_server 使用）
        /// 与 output_pipe（FIFO 模式）可并存
        using FrameCallback = std::function<void(const uint8_t* data, size_t size)>;
        FrameCallback on_frame;

        JpegEncodeType() = default;
    } jpeg_encode;

    // ========================================
    // OpenCV 消费类型（CONSUME_OPENCV）
    // Buffer → cv::Mat 转换后执行指定操作，再计算 PSNR/SSIM
    // ========================================
    struct OpencvType {
        bool enable = false;          ///< 是否启用 OpenCV 消费

        /// 操作类型：决定对 SW 参考帧施加哪种 OpenCV 变换
        enum class OpType {
            NONE,         ///< 无操作，直接比较原始解码帧
            SAVE_LOAD_IMG,///< 保存图片到文件再读取，与原始帧比较（SINGLE 模式）
            ADD,          ///< cv::add 两个 Mat 相加（多生产者对比）
            ABSDIFF,      ///< cv::absdiff 两个 Mat 绝对差（多生产者对比）
            ADD_WEIGHTED, ///< cv::addWeighted 加权求和（多生产者对比）
            BITWISE_AND,  ///< cv::bitwise_and 按位与（多生产者对比）
            BITWISE_OR,   ///< cv::bitwise_or 按位或（多生产者对比）
            BITWISE_XOR,  ///< cv::bitwise_xor 按位异或（多生产者对比）
            BITWISE_NOT,  ///< cv::bitwise_not 按位非（单生产者）
            RESIZE,       ///< cv::resize 缩放
            CROP,         ///< ROI 裁剪（src(cv::Rect(...))）
            ERODE,        ///< cv::erode 腐蚀
            DILATE,       ///< cv::dilate 膨胀
            MORPH_OPEN,   ///< 开运算：先腐蚀后膨胀
            MORPH_CLOSE,  ///< 闭运算：先膨胀后腐蚀
            SOBEL,        ///< cv::Sobel 边缘检测
            CANNY,        ///< cv::Canny 边缘检测
            LAPLACIAN,    ///< cv::Laplacian 拉普拉斯边缘
            TRANSLATE,    ///< cv::warpAffine 平移
            ROTATE,       ///< cv::warpAffine 旋转
            PERSPECTIVE,  ///< cv::warpPerspective 透视变换
            DRAW_LINE,    ///< cv::line 画线
            DRAW_RECT,    ///< cv::rectangle 画矩形
            PUT_TEXT,     ///< cv::putText 绘文字
            GAUSSIAN_BLUR,///< cv::GaussianBlur 高斯模糊
            THRESHOLD,    ///< cv::threshold 二值化
            SPLIT,        ///< cv::split 通道分离
            MERGE,        ///< cv::merge 通道合并
            CVTCOLOR,     ///< cv::cvtColor 颜色空间转换
        };
        OpType op_type = OpType::NONE;

        // ----------------------------------------
        // cv::resize 参数
        // ----------------------------------------
        struct Resize {
            int    dst_width;    ///< 目标宽度（像素，0 = 由 fx 决定）
            int    dst_height;    ///< 目标高度（像素，0 = 由 fy 决定）
            double fx;  ///< 水平缩放因子（0 = 由 dst_width 决定）
            double fy;  ///< 垂直缩放因子（0 = 由 dst_height 决定）
            int    interpolation;    ///< 插值方法（默认 1 = INTER_LINEAR）
            Resize() = default;
        } resize;

        // ----------------------------------------
        // ROI 裁剪参数
        // ----------------------------------------
        struct Crop {
            int x;  ///< 裁剪起始 X 坐标（像素）
            int y;  ///< 裁剪起始 Y 坐标（像素）
            int width;  ///< 裁剪区域宽度（像素）
            int height;  ///< 裁剪区域高度（像素）
            Crop() = default;
        } crop;

        // ----------------------------------------
        // 形态学操作参数（erode / dilate / open / close）
        // ----------------------------------------
        struct Morph {
            int kernel_size  = 3;  ///< 结构元素边长（像素，需为奇数）
            int kernel_shape = 0;  ///< 结构元素形状（MORPH_RECT=0）
            int anchor_x     = -1; ///< 锚点 X（-1 = 中心）
            int anchor_y     = -1; ///< 锚点 Y（-1 = 中心）
            int iterations   = 1;  ///< 迭代次数
            Morph() = default;
        } morph;

        // ----------------------------------------
        // cv::Sobel 边缘检测参数
        // ----------------------------------------
        struct Sobel {
            int    dx    = 1;   ///< X 方向导数阶数
            int    dy    = 0;   ///< Y 方向导数阶数
            int    ksize = 3;   ///< Sobel 核大小（1/3/5/7）
            double scale = 1.0; ///< 缩放因子
            double delta = 0.0; ///< 加到结果的偏移值
            Sobel() = default;
        } sobel;

        // ----------------------------------------
        // cv::Canny 边缘检测参数
        // ----------------------------------------
        struct Canny {
            double threshold1    = 100.0; ///< 低阈值
            double threshold2    = 200.0; ///< 高阈值
            int    aperture_size = 3;     ///< Sobel 核大小（3/5/7）
            Canny() = default;
        } canny;

        // ----------------------------------------
        // cv::Laplacian 拉普拉斯边缘参数
        // ----------------------------------------
        struct Laplacian {
            int    ksize = 1;   ///< 核大小（正奇数）
            double scale = 1.0;
            double delta = 0.0;
            Laplacian() = default;
        } laplacian;

        // ----------------------------------------
        // cv::warpAffine 平移参数
        // ----------------------------------------
        struct Translate {
            double tx = 0.0; ///< X 方向平移量（像素）
            double ty = 0.0; ///< Y 方向平移量（像素）
            Translate() = default;
        } translate;

        // ----------------------------------------
        // cv::warpAffine 旋转参数
        // ----------------------------------------
        struct Rotate {
            double angle = 0.0; ///< 旋转角度（度，逆时针为正）
            double scale = 1.0; ///< 缩放因子
            Rotate() = default;
        } rotate;

        // ----------------------------------------
        // cv::warpPerspective 透视变换参数
        // ----------------------------------------
        struct Perspective {
            int offset = 50; ///< 右上角、左上角的透视偏移量（像素）
            Perspective() = default;
        } perspective;

        // ----------------------------------------
        // cv::line 画线参数
        // ----------------------------------------
        struct DrawLine {
            int x1        = 0;   ///< 起点 X
            int y1        = 0;   ///< 起点 Y
            int x2        = 100; ///< 终点 X
            int y2        = 100; ///< 终点 Y
            int thickness = 2;   ///< 线宽（像素）
            DrawLine() = default;
        } draw_line;

        // ----------------------------------------
        // cv::rectangle 画矩形参数
        // ----------------------------------------
        struct DrawRect {
            int x         = 100; ///< 左上角 X
            int y         = 100; ///< 左上角 Y
            int width     = 200; ///< 宽度
            int height    = 200; ///< 高度
            int thickness = 2;   ///< 线宽（-1=填充）
            DrawRect() = default;
        } draw_rect;

        // ----------------------------------------
        // cv::putText 绘文字参数
        // ----------------------------------------
        struct PutText {
            int    x          = 10;  ///< 文字起始 X
            int    y          = 50;  ///< 文字起始 Y（基线）
            double font_scale = 1.0; ///< 字体缩放
            int    thickness  = 2;   ///< 字体线宽
            PutText() = default;
        } put_text;

        // ----------------------------------------
        // cv::GaussianBlur 高斯模糊参数
        // ----------------------------------------
        struct GaussianBlur {
            int    ksize   = 5;   ///< 核大小（正奇数）
            double sigma_x = 0.0; ///< X 方向标准差（0=由 ksize 自动计算）
            GaussianBlur() = default;
        } gaussian_blur;

        // ----------------------------------------
        // cv::threshold 二值化参数
        // ----------------------------------------
        struct Threshold {
            double thresh = 128.0; ///< 阈值
            double maxval = 255.0; ///< 最大值
            int    type   = 0;     ///< 阈值类型（默认 THRESH_BINARY）
            Threshold() = default;
        } threshold;

        // ----------------------------------------
        // cv::split / cv::merge 通道分离合并参数
        // ----------------------------------------
        struct SplitMerge {
            int    channels = 3;     ///< 通道数（3=BGR, 4=BGRA）
            SplitMerge() = default;
        } split_merge;

        // ----------------------------------------
        // cv::cvtColor 颜色空间转换参数
        // ----------------------------------------
        struct ColorConvert {
            int    code = 0;         ///< cv::ColorConversionCodes
            int    dstCn = 0;        ///< 目标通道数（0=自动）
            ColorConvert() = default;
        } cvtcolor;

        std::unique_ptr<IOpencvVendorExtension> vendor;

        OpencvType() = default;
        OpencvType(const OpencvType& o);
        OpencvType& operator=(const OpencvType& o);
        OpencvType(OpencvType&&) = default;
        OpencvType& operator=(OpencvType&&) = default;
    } opencv;
    
    /**
     * @brief 从 shared config 继承伴随消费者设置
     *
     * 当驱动插件（save / vdec / pp）的 buildPipelineConfigs 创建全新的
     * WorkerConfig 时，伴随插件（display / npu）通过 applyTo 写入 shared
     * config 的设置不会自动传播。本方法将 shared 中已启用、但本配置中
     * 未启用的伴随消费设置补充过来。
     *
     * @param shared applyTo 阶段产出的共享 ConsumerTypeConfig
     */
    void inheritCompanionSettings(const ConsumerTypeConfig& shared);

    ConsumerTypeConfig() = default;
    ConsumerTypeConfig(const ConsumerTypeConfig&) = default;
    ConsumerTypeConfig& operator=(const ConsumerTypeConfig&) = default;
    ConsumerTypeConfig(ConsumerTypeConfig&&) = default;
    ConsumerTypeConfig& operator=(ConsumerTypeConfig&&) = default;
};

#endif // CONSUMPTIONLINE_CONFIG_CONSUMER_TYPE_CONFIG_HPP
