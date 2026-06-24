#pragma once

#include "bufferpool/buffer/Buffer.hpp"
#include "common/ImageMeta.hpp"
#include "consumptionline/config/ConsumerTypeConfig.hpp"
#include "productionline/worker/config/MultiWorkerConfig.hpp"
#include "productionline/worker/config/WorkerConfigs.hpp"
#include <string>
#include <vector>
#include <atomic>
#include <cstdio>
#include "opencv2/core.hpp"

// FFmpeg前向声明
extern "C" {
#include <libavutil/pixfmt.h>
}

struct AVFrame;
struct SwsContext;
struct AVPixFmtDescriptor;

namespace consumptionline {
namespace io {

/**
 * @brief 对比配置（类型别名）
 * 
 * CompareConfig 现在统一定义在 WorkerConfig::ConsumerTypeConfig::CompareType 中，
 * 此处提供类型别名以保持 API 兼容性。
 */
using CompareConfig = WorkerConfig::ConsumerTypeConfig::CompareType;

/**
 * @brief 单帧对比结果
 */
struct FrameCompareResult {
    int frame_index = 0;
    bool passed = false;
    
    // ========== PSNR结果 ==========
    // YUV格式：psnr_y/u/v 表示 Y/U/V 平面
    // RGB格式：复用字段 psnr_y=G, psnr_u=R, psnr_v=B
    double psnr_y = 0.0;    // Y平面 或 G通道（主要指标）
    double psnr_u = 0.0;    // U平面 或 R通道
    double psnr_v = 0.0;    // V平面 或 B通道
    double psnr_avg = 0.0;  // 加权平均
    
    // ========== SSIM结果 ==========
    // YUV格式：ssim_y/u/v 表示 Y/U/V 平面
    // RGB格式：复用字段 ssim_y=G, ssim_u=R, ssim_v=B
    double ssim_y = 0.0;    // Y平面 或 G通道（主要指标）范围 [0.0, 1.0]
    double ssim_u = 0.0;    // U平面 或 R通道，范围 [0.0, 1.0]
    double ssim_v = 0.0;    // V平面 或 B通道，范围 [0.0, 1.0]
    double ssim_avg = 0.0;  // 加权平均，范围 [0.0, 1.0]
    
    // ========== 像素差异统计 ==========
    int max_pixel_diff = 0;      // 最大像素差值
    int diff_pixel_count = 0;    // 差异像素数量
    float diff_pixel_ratio = 0.0; // 差异像素比例
    
    // ========== 判定级别 ==========
    enum Level {
        PASS,    // 通过
        WARN,    // 警告（边界）
        FAIL     // 失败
    } level = PASS;

    // ========== 颜色空间（用于日志/统计标签） ==========
    enum ColorSpace {
        COLOR_UNKNOWN,
        COLOR_YUV,
        COLOR_RGB,
        COLOR_BGR,
        COLOR_MIXED
    } color_space = COLOR_UNKNOWN;
    
    std::string error_message;
    
    // ========== 格式信息 ==========
    std::string ref_format_name;
    std::string test_format_name;
};

/**
 * @brief Buffer对比器（IO后处理，支持多格式）
 * 
 * 架构角色：IO后处理（与BufferWriter并列）
 * 
 * 职责：
 * - 对比两个解码器的输出Buffer（参考 vs 测试）
 * - 自动检测格式并选择最优对比策略
 * - 支持YUV/RGB格式及混合格式对比
 * - 计算PSNR等质量指标
 * - 收集统计数据并生成报告
 * 
 * 使用方式（类似BufferWriter）：
 * ```cpp
 * using namespace consumptionline::io;
 * 
 * BufferComparator comparator;
 * CompareConfig config;  // CompareConfig = WorkerConfig::ConsumerTypeConfig::CompareType
 * config.enable_psnr = true;
 * config.strategy = CompareConfig::AUTO_LAYERED;
 * config.format_strategy = CompareConfig::AUTO;
 * config.min_psnr = 38.0;  // >= 38dB 通过
 * 
 * comparator.open(config);
 * 
 * while (running) {
 *     Buffer* sw_buf = sw_pool->acquireFilled(...);
 *     Buffer* hw_buf = hw_pool->acquireFilled(...);
 *     
 *     FrameCompareResult result = comparator.compare(sw_buf, hw_buf);
 *     
 *     sw_pool->releaseFilled(sw_buf);
 *     hw_pool->releaseFilled(hw_buf);
 * }
 * 
 * comparator.close();
 * comparator.printSummary();
 * ```
 * 
 * 特性：
 * - 格式自适应：自动检测YUV/RGB并选择算法
 * - 2层验证：快速验证（PSNR-Y/G）→ 深度验证（完整PSNR）
 * - 零转换优化：相同格式直接对比
 * - 感知加权：Y/G通道权重更高
 */
class BufferComparator {
public:
    BufferComparator();
    ~BufferComparator();
    
    // 禁止拷贝
    BufferComparator(const BufferComparator&) = delete;
    BufferComparator& operator=(const BufferComparator&) = delete;
    
    /**
     * @brief 打开对比器（初始化）
     * @param config 对比配置
     * @return 成功返回true
     */
    bool open(const CompareConfig& config = CompareConfig());
    
    /**
     * @brief 对比两个Buffer
     * @param reference_buffer 参考Buffer（通常是软件解码）
     * @param test_buffer 测试Buffer（通常是硬件解码）
     * @return 对比结果
     * 
     * @note 自动推断帧索引（从0开始递增）
     * @note 线程安全（内部使用原子计数）
     * @note 自动检测格式并选择最优对比策略
     */
    FrameCompareResult compare(Buffer* reference_buffer, Buffer* test_buffer);

    /**
     * @brief 对比两个 AVFrame（用于 PTS 匹配的深拷贝帧比较）
     * @param ref_frame 参考帧
     * @param test_frame 测试帧
     * @return 对比结果
     */
    FrameCompareResult compareAVFrames(AVFrame* ref_frame, AVFrame* test_frame);
    
    /**
     * @brief 关闭对比器（输出报告）
     */
    void close();
    
    /**
     * @brief 打印统计摘要（类似BufferWriter::printStats）
     */
    void printSummary() const;
    
private:
    CompareConfig config_;
    bool is_open_;
    
    // ========== 统计信息（原子变量，线程安全）==========
    std::atomic<int> compare_count_;
    std::atomic<int> passed_count_;
    std::atomic<int> warned_count_;
    std::atomic<int> failed_count_;
    
    // ========== 质量统计 ==========
    double sum_psnr_y_;
    double sum_psnr_u_;
    double sum_psnr_v_;
    double min_psnr_y_;
    double max_psnr_y_;
    
    double sum_ssim_y_;
    double sum_ssim_u_;
    double sum_ssim_v_;
    double min_ssim_y_;
    double max_ssim_y_;
    
    FrameCompareResult::ColorSpace stats_color_space_;
    
    // ========== 失败帧列表 ==========
    std::vector<FrameCompareResult> failures_;
    std::vector<FrameCompareResult> warnings_;
    
    // ========== 报告文件 ==========
    FILE* report_file_;
    
    // ========== 格式信息 ==========
    struct FormatInfo {
        AVPixelFormat format;
        bool is_yuv;
        bool is_rgb;
        bool is_bgr;      // BGR 格式（如 BGR24, BGRA 等）
        bool is_planar;
        bool is_mat = false;   // Buffer 中存储的是 cv::Mat（非 AVFrame）
        int num_planes;
        int width;
        int height;
        std::string name;
    };
    
    /**
     * @brief 分析图像格式（从 ImageMeta 提取格式分类信息）
     */
    FormatInfo analyzeFormat(const ImageMeta& img);
    
    // ========== 多格式对比策略（核心）==========
    // 所有策略函数接受 const ImageMeta& 而非 Buffer*，
    // 使得 compareAVFrames() 可直接从 AVFrame 构建 ImageMeta 传入，
    // 无需创建临时 AVFrameBuffer 包装器。
    
    /**
     * @brief 自动选择对比策略
     */
    FrameCompareResult compareAuto(
        const ImageMeta& ref_img, const FormatInfo& ref_info,
        const ImageMeta& test_img, const FormatInfo& test_info
    );
    
    /**
     * @brief YUV格式对比（分平面）
     */
    FrameCompareResult compareYUV(
        const ImageMeta& ref_img, const FormatInfo& ref_info,
        const ImageMeta& test_img, const FormatInfo& test_info
    );
    
    /**
     * @brief RGB格式对比（分通道）
     */
    FrameCompareResult compareRGB(
        const ImageMeta& ref_img, const FormatInfo& ref_info,
        const ImageMeta& test_img, const FormatInfo& test_info
    );

    /**
     * @brief BGR格式对比（不区分策略，三个通道全部计算）
     */
    FrameCompareResult compareBGR(
        const ImageMeta& ref_img, const FormatInfo& ref_info,
        const ImageMeta& test_img, const FormatInfo& test_info
    );

    /**
     * @brief 混合格式对比（需要转换）
     */
    FrameCompareResult compareMixed(
        const ImageMeta& ref_img, const FormatInfo& ref_info,
        const ImageMeta& test_img, const FormatInfo& test_info
    );
    
    /**
     * @brief 子字节 packed RGB 格式对比（RGB444/555/565 等）
     * 先将两帧转为 RGB24 再调用 compareRGB
     */
    FrameCompareResult compareSubByteRGB(
        const ImageMeta& ref_img, const FormatInfo& ref_info,
        const ImageMeta& test_img, const FormatInfo& test_info
    );
    
    // ========== PSNR计算 ==========
    
    /**
     * @brief 通用PSNR计算（单平面/通道）
     */
    double calculatePSNR(
        const uint8_t* data1, const uint8_t* data2,
        int width, int height,
        int stride1, int stride2
    );
    
    /**
     * @brief YUV格式：Y平面PSNR
     */
    double calculatePSNR_YUV_Y(const ImageMeta& img1, const ImageMeta& img2, 
                               const FormatInfo& info1, const FormatInfo& info2);
    
    /**
     * @brief YUV格式：U平面PSNR
     */
    double calculatePSNR_YUV_U(const ImageMeta& img1, const ImageMeta& img2,
                               const FormatInfo& info1, const FormatInfo& info2);
    
    /**
     * @brief YUV格式：V平面PSNR
     */
    double calculatePSNR_YUV_V(const ImageMeta& img1, const ImageMeta& img2,
                               const FormatInfo& info1, const FormatInfo& info2);
    
    /**
     * @brief RGB格式：R通道PSNR
     */
    double calculatePSNR_RGB_R(const ImageMeta& img1, const ImageMeta& img2,
                               const FormatInfo& info1, const FormatInfo& info2);
    
    /**
     * @brief RGB格式：G通道PSNR
     */
    double calculatePSNR_RGB_G(const ImageMeta& img1, const ImageMeta& img2,
                               const FormatInfo& info1, const FormatInfo& info2);
    
    /**
     * @brief RGB格式：B通道PSNR
     */
    double calculatePSNR_RGB_B(const ImageMeta& img1, const ImageMeta& img2,
                               const FormatInfo& info1, const FormatInfo& info2);
    
    // ========== SSIM计算 ==========
    
    /**
     * @brief 通用 SSIM 计算（单平面/通道）
     * @param data1 第一个图像数据
     * @param data2 第二个图像数据
     * @param width 图像宽度
     * @param height 图像高度
     * @param stride1 第一个图像的行跨度
     * @param stride2 第二个图像的行跨度
     * @return SSIM 值，范围 [0.0, 1.0]，1.0 表示完全相同
     * 
     * @note 使用 8x8 滑动窗口和标准 SSIM 算法
     * @note 计算复杂度约为 PSNR 的 1.5-2 倍
     */
    double calculateSSIM(
        const uint8_t* data1, const uint8_t* data2,
        int width, int height,
        int stride1, int stride2
    );
    
    /**
     * @brief YUV格式：Y平面 SSIM
     */
    double calculateSSIM_YUV_Y(const ImageMeta& img1, const ImageMeta& img2, 
                              const FormatInfo& info1, const FormatInfo& info2);
    
    /**
     * @brief YUV格式：U平面 SSIM
     */
    double calculateSSIM_YUV_U(const ImageMeta& img1, const ImageMeta& img2,
                              const FormatInfo& info1, const FormatInfo& info2);
    
    /**
     * @brief YUV格式：V平面 SSIM
     */
    double calculateSSIM_YUV_V(const ImageMeta& img1, const ImageMeta& img2,
                              const FormatInfo& info1, const FormatInfo& info2);
    
    /**
     * @brief RGB格式：R通道 SSIM
     */
    double calculateSSIM_RGB_R(const ImageMeta& img1, const ImageMeta& img2,
                              const FormatInfo& info1, const FormatInfo& info2);
    
    /**
     * @brief RGB格式：G通道 SSIM
     */
    double calculateSSIM_RGB_G(const ImageMeta& img1, const ImageMeta& img2,
                              const FormatInfo& info1, const FormatInfo& info2);
    
    /**
     * @brief RGB格式：B通道 SSIM
     */
    double calculateSSIM_RGB_B(const ImageMeta& img1, const ImageMeta& img2,
                              const FormatInfo& info1, const FormatInfo& info2);

    // ========== 辅助方法 ==========
    
    /**
     * @brief 元数据对比
     */
    bool compareMetadata(
        const FormatInfo& ref_info,
        const FormatInfo& test_info,
        FrameCompareResult& result
    );
    
    /**
     * @brief 更新统计信息
     */
    void updateStatistics(const FrameCompareResult& result);
    
    /**
     * @brief 写入报告
     */
    void writeReport(const FrameCompareResult& result);
    
    /**
     * @brief pp模式分辨率不匹配标记
     * 当参考帧和测试帧分辨率不同时设为true，
     * 后续对比会先将参考帧缩放到测试帧分辨率
     */
    bool resolution_mismatch_detected_ = false;
    
    /**
     * @brief 将AVFrame缩放到目标分辨率
     * @param src_frame 源帧
     * @param dst_width 目标宽度
     * @param dst_height 目标高度
     * @return 缩放后的AVFrame，失败返回nullptr（调用者负责释放）
     */
    AVFrame* rescaleFrame(AVFrame* src_frame, int dst_width, int dst_height);
    
    /**
     * @brief 格式转换到YUV420P（从 ImageMeta 读取源帧）
     */
    AVFrame* convertToYUV420P(const ImageMeta& img, const FormatInfo& info);
    
    /**
     * @brief 将YUV格式转换为RGB格式（用于RGB对比）
     * @param img 源图像信息
     * @param info YUV格式信息
     * @param target_rgb_format 目标RGB格式
     * @return 转换后的AVFrame，失败返回nullptr
     */
    AVFrame* convertYUVToRGB(const ImageMeta& img, const FormatInfo& info, AVPixelFormat target_rgb_format);
    
    /**
     * @brief 将子字节 packed RGB 格式转为 RGB24
     * @param img 源图像信息（RGB444/555/565 等）
     * @param info 格式信息
     * @return 转换后的 RGB24 AVFrame，调用者负责释放
     */
    AVFrame* convertToRGB24(const ImageMeta& img, const FormatInfo& info);
    
    /**
     * @brief 释放转换后的AVFrame
     */
    void freeConvertedFrame(AVFrame* frame);
};

} // namespace io
} // namespace consumptionline
