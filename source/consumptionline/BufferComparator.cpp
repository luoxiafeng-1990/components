#include "consumptionline/types/compare/BufferComparator.hpp"
#include "common/ImageMeta.hpp"
#include "common/Logger.hpp"
#include "common/GlobalThreadPool.hpp"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <future>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace consumptionline {
namespace io {

// ============================================================================
// 构造/析构
// ============================================================================

BufferComparator::BufferComparator()
    : is_open_(false)
    , compare_count_(0)
    , passed_count_(0)
    , warned_count_(0)
    , failed_count_(0)
    , sum_psnr_y_(0.0)
    , sum_psnr_u_(0.0)
    , sum_psnr_v_(0.0)
    , min_psnr_y_(100.0)
    , max_psnr_y_(0.0)
    , sum_ssim_y_(0.0)
    , sum_ssim_u_(0.0)
    , sum_ssim_v_(0.0)
    , min_ssim_y_(1.0)
    , max_ssim_y_(0.0)
    , report_file_(nullptr)
{
}

BufferComparator::~BufferComparator() {
    if (is_open_) {
        close();
    }
}

// ============================================================================
// 公共接口
// ============================================================================

bool BufferComparator::open(const CompareConfig& config) {
    if (is_open_) {
        LOG_WARN("[BufferComparator] Already opened");
        return false;
    }
    
    config_ = config;
    
    // 检查：至少启用一个指标
    if (!config_.enable_psnr && !config_.enable_ssim) {
        LOG_ERROR("[BufferComparator] At least one metric (PSNR or SSIM) must be enabled");
        return false;
    }
    
    // 打开报告文件
    if (config_.save_report) {
        report_file_ = fopen(config_.report_path.c_str(), "w");
        if (!report_file_) {
            LOG_ERROR_FMT("[BufferComparator] Failed to open report file: %s", 
                         config_.report_path.c_str());
            return false;
        }
        
        // 写入报告头
        fprintf(report_file_, "═══════════════════════════════════════════════════════\n");
        fprintf(report_file_, "  Decoder Comparison Report\n");
        fprintf(report_file_, "═══════════════════════════════════════════════════════\n");
        fprintf(report_file_, "Strategy: %s\n", 
                config_.strategy == CompareConfig::FAST_ONLY ? "FAST_ONLY" :
                config_.strategy == CompareConfig::AUTO_LAYERED ? "AUTO_LAYERED" : "DEEP_ALWAYS");
        fprintf(report_file_, "Format Strategy: %s\n",
                config_.format_strategy == CompareConfig::AUTO ? "AUTO" :
                config_.format_strategy == CompareConfig::FORCE_YUV ? "FORCE_YUV" :
                config_.format_strategy == CompareConfig::FORCE_RGB ? "FORCE_RGB" : "NATIVE");
        fprintf(report_file_, "Enabled Metrics: %s%s%s\n",
                config_.enable_psnr ? "PSNR" : "",
                (config_.enable_psnr && config_.enable_ssim) ? " + " : "",
                config_.enable_ssim ? "SSIM" : "");
        fprintf(report_file_, "Parallel Computing: %s\n",
                config_.enable_parallel ? "Enabled (using GlobalThreadPool)" : "Disabled");
        if (config_.enable_psnr) {
            fprintf(report_file_, "PSNR Threshold: Pass >= %.1f dB, Warn >= %.1f dB\n",
                    config_.min_psnr, config_.warn_psnr);
        }
        if (config_.enable_ssim) {
            fprintf(report_file_, "SSIM Threshold: Pass >= %.4f, Warn >= %.4f\n",
                    config_.min_ssim, config_.warn_ssim);
        }
        fprintf(report_file_, "═══════════════════════════════════════════════════════\n\n");
        fflush(report_file_);
    }
    
    // 重置统计
    compare_count_ = 0;
    passed_count_ = 0;
    warned_count_ = 0;
    failed_count_ = 0;
    sum_psnr_y_ = 0.0;
    sum_psnr_u_ = 0.0;
    sum_psnr_v_ = 0.0;
    min_psnr_y_ = 100.0;
    max_psnr_y_ = 0.0;
    sum_ssim_y_ = 0.0;
    sum_ssim_u_ = 0.0;
    sum_ssim_v_ = 0.0;
    min_ssim_y_ = 1.0;
    max_ssim_y_ = 0.0;
    failures_.clear();
    warnings_.clear();
    
    is_open_ = true;
    LOG_INFO("[BufferComparator] Opened successfully");
    return true;
}

void BufferComparator::close() {
    if (!is_open_) {
        return;
    }
    
    // 写入报告尾
    if (report_file_) {
        fprintf(report_file_, "\n═══════════════════════════════════════════════════════\n");
        fprintf(report_file_, "  Summary\n");
        fprintf(report_file_, "═══════════════════════════════════════════════════════\n");
        fprintf(report_file_, "Total frames compared: %d\n", compare_count_.load());
        fprintf(report_file_, "Passed: %d (%.1f%%)\n", 
                passed_count_.load(), 
                compare_count_ > 0 ? 100.0 * passed_count_.load() / compare_count_.load() : 0.0);
        fprintf(report_file_, "Warned: %d (%.1f%%)\n",
                warned_count_.load(),
                compare_count_ > 0 ? 100.0 * warned_count_.load() / compare_count_.load() : 0.0);
        fprintf(report_file_, "Failed: %d (%.1f%%)\n",
                failed_count_.load(),
                compare_count_ > 0 ? 100.0 * failed_count_.load() / compare_count_.load() : 0.0);
        
        if (compare_count_ > 0) {
            if (config_.enable_psnr) {
                fprintf(report_file_, "\nPSNR Statistics:\n");
                fprintf(report_file_, "  Average: Y=%.2f U=%.2f V=%.2f dB\n",
                        sum_psnr_y_ / compare_count_.load(),
                        sum_psnr_u_ / compare_count_.load(),
                        sum_psnr_v_ / compare_count_.load());
                fprintf(report_file_, "  Min Y: %.2f dB\n", min_psnr_y_);
                fprintf(report_file_, "  Max Y: %.2f dB\n", max_psnr_y_);
            }
            
            if (config_.enable_ssim) {
                fprintf(report_file_, "\nSSIM Statistics:\n");
                fprintf(report_file_, "  Average: Y=%.4f U=%.4f V=%.4f\n",
                        sum_ssim_y_ / compare_count_.load(),
                        sum_ssim_u_ / compare_count_.load(),
                        sum_ssim_v_ / compare_count_.load());
                fprintf(report_file_, "  Min Y: %.4f\n", min_ssim_y_);
                fprintf(report_file_, "  Max Y: %.4f\n", max_ssim_y_);
            }
        }
        
        fprintf(report_file_, "\n%s\n", 
                failed_count_.load() == 0 ? "✅ ALL TESTS PASSED" : "❌ SOME TESTS FAILED");
        fprintf(report_file_, "═══════════════════════════════════════════════════════\n");
        
        fclose(report_file_);
        report_file_ = nullptr;
        
        LOG_INFO_FMT("[BufferComparator] Report saved to: %s", config_.report_path.c_str());
    }
    
    is_open_ = false;
    LOG_INFO("[BufferComparator] Closed");
}

FrameCompareResult BufferComparator::compare(
    Buffer* reference_buffer, 
    Buffer* test_buffer
) {
    FrameCompareResult result;
    result.frame_index = compare_count_++;
    
    if (!is_open_) {
        LOG_ERROR("[BufferComparator] Not opened");
        result.error_message = "Comparator not opened";
        result.passed = false;
        result.level = FrameCompareResult::FAIL;
        return result;
    }
    
    if (!reference_buffer || !test_buffer) {
        LOG_ERROR("[BufferComparator] Null buffer");
        result.error_message = "Null buffer";
        result.passed = false;
        result.level = FrameCompareResult::FAIL;
        return result;
    }
    
    // 步骤1：在入口一次性提取 ImageMeta，后续内部函数不再接触 Buffer*
    ImageMeta ref_img = ImageMeta::fromBuffer(reference_buffer);
    ImageMeta test_img = ImageMeta::fromBuffer(test_buffer);
    
    FormatInfo ref_info = analyzeFormat(ref_img);
    FormatInfo test_info = analyzeFormat(test_img);
    
    result.ref_format_name = ref_info.name;
    result.test_format_name = test_info.name;
    
    if (config_.verbose && result.frame_index == 0) {
        LOG_INFO_FMT("[BufferComparator] Frame %d format detected:", result.frame_index);
        LOG_INFO_FMT("  Reference: %s (%dx%d, %s, %d planes)", 
                     ref_info.name.c_str(), ref_info.width, ref_info.height,
                     ref_info.is_yuv ? "YUV" : ref_info.is_rgb ? "RGB" : "Unknown",
                     ref_info.num_planes);
        LOG_INFO_FMT("  Test:      %s (%dx%d, %s, %d planes)",
                     test_info.name.c_str(), test_info.width, test_info.height,
                     test_info.is_yuv ? "YUV" : test_info.is_rgb ? "RGB" : "Unknown",
                     test_info.num_planes);
    }
    
    // 步骤2：元数据检查
    resolution_mismatch_detected_ = false;
    if (!compareMetadata(ref_info, test_info, result)) {
        failed_count_++;
        updateStatistics(result);
        writeReport(result);
        return result;
    }
    
    // 步骤2.5：分辨率不匹配时（pp/scale场景），转 YUV420P 后缩放参考帧再比较
    if (resolution_mismatch_detected_) {
        AVFrame* ref_yuv = convertToYUV420P(ref_img, ref_info);
        AVFrame* test_yuv = convertToYUV420P(test_img, test_info);
        
        if (!ref_yuv || !test_yuv) {
            if (ref_yuv) freeConvertedFrame(ref_yuv);
            if (test_yuv) freeConvertedFrame(test_yuv);
            result.error_message = "Format conversion failed for resolution-mismatch path";
            result.passed = false;
            result.level = FrameCompareResult::FAIL;
            failed_count_++;
            updateStatistics(result);
            writeReport(result);
            return result;
        }
        
        AVFrame* ref_scaled = rescaleFrame(ref_yuv, test_yuv->width, test_yuv->height);
        freeConvertedFrame(ref_yuv);
        
        if (!ref_scaled) {
            freeConvertedFrame(test_yuv);
            result.error_message = "Rescale failed for resolution-mismatch path";
            result.passed = false;
            result.level = FrameCompareResult::FAIL;
            failed_count_++;
            updateStatistics(result);
            writeReport(result);
            return result;
        }
        
        // 直接从转换后的 AVFrame 构建 ImageMeta，无需临时 AVFrameBuffer
        ImageMeta scaled_img = ImageMeta::fromAVFrame(ref_scaled);
        ImageMeta test_yuv_img = ImageMeta::fromAVFrame(test_yuv);
        FormatInfo scaled_info = analyzeFormat(scaled_img);
        FormatInfo test_yuv_fi = analyzeFormat(test_yuv_img);
        
        result = compareYUV(scaled_img, scaled_info, test_yuv_img, test_yuv_fi);
        
        freeConvertedFrame(ref_scaled);
        freeConvertedFrame(test_yuv);
        
        result.frame_index = compare_count_.load() - 1;
        updateStatistics(result);
        writeReport(result);
        
        if (config_.verbose) {
            if (result.level == FrameCompareResult::FAIL) {
                LOG_ERROR_FMT("  Frame %d (rescaled): FAIL (PSNR-Y: %.2f dB)", result.frame_index, result.psnr_y);
            } else if (result.level == FrameCompareResult::WARN) {
                LOG_WARN_FMT("  Frame %d (rescaled): WARN (PSNR-Y: %.2f dB)", result.frame_index, result.psnr_y);
            } else if (result.frame_index % 50 == 0) {
                LOG_DEBUG_FMT("  Frame %d (rescaled): PASS (PSNR-Y: %.2f dB)", result.frame_index, result.psnr_y);
            }
        }
        
        return result;
    }
    
    // 步骤3：根据配置选择对比策略
    switch (config_.format_strategy) {
        case CompareConfig::AUTO:
            result = compareAuto(ref_img, ref_info, test_img, test_info);
            break;
            
        case CompareConfig::FORCE_YUV:
            result = compareMixed(ref_img, ref_info, test_img, test_info);
            break;
            
        case CompareConfig::NATIVE:
            // Mat 没有 AVPixelFormat，跳过格式一致性检查，直接按 is_yuv/is_rgb 路由
            if (!ref_info.is_mat && !test_info.is_mat &&
                ref_info.format != test_info.format) {
                result.error_message = "Format mismatch in NATIVE mode";
                result.passed = false;
                result.level = FrameCompareResult::FAIL;
                failed_count_++;
                updateStatistics(result);
                writeReport(result);
                return result;
            }
            
            if (ref_info.is_yuv) {
                result = compareYUV(ref_img, ref_info, test_img, test_info);
            } else if (ref_info.is_rgb) {
                result = compareRGB(ref_img, ref_info, test_img, test_info);
            }
            break;
            
        default:
            result.error_message = "Unknown format strategy";
            result.passed = false;
            result.level = FrameCompareResult::FAIL;
            failed_count_++;
            break;
    }
    
    result.frame_index = compare_count_.load() - 1;  // 修正索引
    updateStatistics(result);
    writeReport(result);
    
    // ⭐ 统一在 compare() 返回前打印日志（修复 frame_index 不一致问题）
    if (config_.verbose) {
        // 只在失败/警告时打印，或每 50 帧打印一次进度
        if (result.level == FrameCompareResult::FAIL) {
            if (config_.enable_psnr && config_.enable_ssim) {
                LOG_ERROR_FMT("  Frame %d: FAIL (PSNR-Y: %.2f dB, SSIM-Y: %.4f)", 
                             result.frame_index, result.psnr_y, result.ssim_y);
            } else if (config_.enable_psnr) {
                LOG_ERROR_FMT("  Frame %d: FAIL (PSNR-Y: %.2f dB)", 
                             result.frame_index, result.psnr_y);
            } else {
                LOG_ERROR_FMT("  Frame %d: FAIL (SSIM-Y: %.4f)", 
                             result.frame_index, result.ssim_y);
            }
        } else if (result.level == FrameCompareResult::WARN) {
            if (config_.enable_psnr && config_.enable_ssim) {
                LOG_WARN_FMT("  Frame %d: WARN (PSNR-Y: %.2f dB, SSIM-Y: %.4f)", 
                            result.frame_index, result.psnr_y, result.ssim_y);
            } else if (config_.enable_psnr) {
                LOG_WARN_FMT("  Frame %d: WARN (PSNR-Y: %.2f dB)", 
                            result.frame_index, result.psnr_y);
            } else {
                LOG_WARN_FMT("  Frame %d: WARN (SSIM-Y: %.4f)", 
                            result.frame_index, result.ssim_y);
            }
        } else if (result.frame_index % 50 == 0) {
            // PASS: 每 50 帧打印一次进度
            if (config_.enable_psnr && config_.enable_ssim) {
                LOG_DEBUG_FMT("  Frame %d: PASS (PSNR-Y: %.2f dB, SSIM-Y: %.4f)", 
                             result.frame_index, result.psnr_y, result.ssim_y);
            } else if (config_.enable_psnr) {
                LOG_DEBUG_FMT("  Frame %d: PASS (PSNR-Y: %.2f dB)", 
                             result.frame_index, result.psnr_y);
            } else {
                LOG_DEBUG_FMT("  Frame %d: PASS (SSIM-Y: %.4f)", 
                             result.frame_index, result.ssim_y);
            }
        }
    }
    
    return result;
}

FrameCompareResult BufferComparator::compareAVFrames(
    AVFrame* ref_frame,
    AVFrame* test_frame
) {
    FrameCompareResult result;
    if (!is_open_ || !ref_frame || !test_frame) {
        result.error_message = !is_open_ ? "Not opened" : "Null AVFrame";
        result.passed = false;
        result.level = FrameCompareResult::FAIL;
        return result;
    }

    // 直接从 AVFrame 构建 ImageMeta，不再创建临时 AVFrameBuffer
    ImageMeta ref_img = ImageMeta::fromAVFrame(ref_frame);
    ImageMeta test_img = ImageMeta::fromAVFrame(test_frame);
    
    FormatInfo ref_info = analyzeFormat(ref_img);
    FormatInfo test_info = analyzeFormat(test_img);
    
    result.frame_index = compare_count_++;
    result.ref_format_name = ref_info.name;
    result.test_format_name = test_info.name;

    // 直接路由到策略函数
    switch (config_.format_strategy) {
        case CompareConfig::AUTO:
            result = compareAuto(ref_img, ref_info, test_img, test_info);
            break;
        case CompareConfig::FORCE_YUV:
            result = compareMixed(ref_img, ref_info, test_img, test_info);
            break;
        case CompareConfig::NATIVE:
            if (ref_info.is_yuv)
                result = compareYUV(ref_img, ref_info, test_img, test_info);
            else if (ref_info.is_rgb)
                result = compareRGB(ref_img, ref_info, test_img, test_info);
            break;
        default:
            result.error_message = "Unknown format strategy";
            result.passed = false;
            result.level = FrameCompareResult::FAIL;
            break;
    }

    result.frame_index = compare_count_.load() - 1;
    updateStatistics(result);
    writeReport(result);
    return result;
}

void BufferComparator::printSummary() const {
    LOG_INFO("╔═══════════════════════════════════════════════════════╗");
    LOG_INFO("║  BufferComparator Summary                              ║");
    LOG_INFO("╚═══════════════════════════════════════════════════════╝");
    LOG_INFO_FMT("  Total frames compared: %d", compare_count_.load());
    LOG_INFO_FMT("  Passed: %d ✅ (%.1f%%)", 
                 passed_count_.load(),
                 compare_count_ > 0 ? 100.0 * passed_count_.load() / compare_count_.load() : 0.0);
    LOG_INFO_FMT("  Warned: %d ⚠️  (%.1f%%)",
                 warned_count_.load(),
                 compare_count_ > 0 ? 100.0 * warned_count_.load() / compare_count_.load() : 0.0);
    LOG_INFO_FMT("  Failed: %d ❌ (%.1f%%)",
                 failed_count_.load(),
                 compare_count_ > 0 ? 100.0 * failed_count_.load() / compare_count_.load() : 0.0);
    
    if (compare_count_ > 0) {
        if (config_.enable_psnr) {
            LOG_INFO("");
            LOG_INFO("  PSNR Statistics:");
            LOG_INFO_FMT("    Average: Y=%.2f U=%.2f V=%.2f dB",
                         sum_psnr_y_ / compare_count_.load(),
                         sum_psnr_u_ / compare_count_.load(),
                         sum_psnr_v_ / compare_count_.load());
            LOG_INFO_FMT("    Range:   Y=[%.2f, %.2f] dB", min_psnr_y_, max_psnr_y_);
        }
        
        if (config_.enable_ssim) {
            LOG_INFO("");
            LOG_INFO("  SSIM Statistics:");
            LOG_INFO_FMT("    Average: Y=%.4f U=%.4f V=%.4f",
                         sum_ssim_y_ / compare_count_.load(),
                         sum_ssim_u_ / compare_count_.load(),
                         sum_ssim_v_ / compare_count_.load());
            LOG_INFO_FMT("    Range:   Y=[%.4f, %.4f]", min_ssim_y_, max_ssim_y_);
        }
    }
    
    LOG_INFO("");
    if (failed_count_.load() == 0) {
        LOG_INFO("  ✅ Result: ALL TESTS PASSED");
    } else {
        LOG_WARN_FMT("  ❌ Result: %d TESTS FAILED", failed_count_.load());
    }
    LOG_INFO("╚═══════════════════════════════════════════════════════╝");
}

// ============================================================================
// 格式分析
// ============================================================================

BufferComparator::FormatInfo BufferComparator::analyzeFormat(const ImageMeta& img) {
    FormatInfo info = {};
    info.format = AV_PIX_FMT_NONE;
    info.name = "Unknown";

    // Mat 路径：ImageMeta 来自 fromMat()，format = NONE 但 plane_data[0] 有效
    if (img.format() == AV_PIX_FMT_NONE && img.planeData(0) && img.width() > 0) {
        info.width = img.width();
        info.height = img.height();
        info.num_planes = img.nbPlanes();
        info.is_planar = false;
        info.is_mat = true;
        // Mat 通道数通过 linesize 推算：linesize[0] / width 即每像素字节数
        int bytes_per_pixel = (img.linesize(0) > 0 && img.width() > 0)
                            ? img.linesize(0) / img.width() : 1;
        if (bytes_per_pixel == 1) {
            info.is_yuv = true;
            info.is_rgb = false;
            info.name = "Mat_Gray";
        } else {
            info.is_yuv = false;
            info.is_rgb = true;
            info.name = "Mat_" + std::to_string(bytes_per_pixel) + "ch";
        }
        return info;
    }

    if (!img.isValid()) {
        return info;
    }

    info.format = img.format();
    info.width = img.width();
    info.height = img.height();
    info.name = av_get_pix_fmt_name(info.format);
    
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(info.format);
    if (desc) {
        info.num_planes = desc->nb_components;
        info.is_planar = !(desc->flags & AV_PIX_FMT_FLAG_RGB);
        
        // YUV格式检测（含 JPEG 全范围 YUVJ*，否则易误判为「不支持」导致 PSNR/SSIM 全 0）
        // 含 packed 422（yuyv422 / uyvy422 等）：否则 is_yuv/is_rgb 皆 false，compareAuto 落入 Unsupported
        info.is_yuv = (info.format == AV_PIX_FMT_YUV420P ||
                      info.format == AV_PIX_FMT_YUVJ420P ||
                      info.format == AV_PIX_FMT_NV12 ||
                      info.format == AV_PIX_FMT_NV21 ||
                      info.format == AV_PIX_FMT_YUV422P ||
                      info.format == AV_PIX_FMT_YUVJ422P ||
                      info.format == AV_PIX_FMT_YUV444P ||
                      info.format == AV_PIX_FMT_YUVJ444P ||
                      info.format == AV_PIX_FMT_YUV410P ||
                      info.format == AV_PIX_FMT_YUV411P ||
                      info.format == AV_PIX_FMT_P010LE ||
                      info.format == AV_PIX_FMT_P016LE ||
                      info.format == AV_PIX_FMT_YUYV422 ||
                      info.format == AV_PIX_FMT_UYVY422 ||
                      info.format == AV_PIX_FMT_YVYU422);
        
        // RGB格式检测
        info.is_rgb = (desc->flags & AV_PIX_FMT_FLAG_RGB) != 0;
    }
    
    return info;
}

// ============================================================================
// 元数据对比
// ============================================================================

bool BufferComparator::compareMetadata(
    const FormatInfo& ref_info,
    const FormatInfo& test_info,
    FrameCompareResult& result
) {
    if (ref_info.width != test_info.width || ref_info.height != test_info.height) {
        LOG_WARN_FMT("[BufferComparator] Resolution differs: ref=%dx%d test=%dx%d (pp/scale expected, will rescale ref for comparison)",
                     ref_info.width, ref_info.height, test_info.width, test_info.height);
        resolution_mismatch_detected_ = true;
    }
    
    return true;
}

// ============================================================================
// 自动选择对比策略
// ============================================================================

/**
 * @brief 检查 RGB 格式是否可被 extractRGBChannel 直接提取通道
 * 子字节打包格式（RGB444/555/565 等）每通道不足 8bit，
 * 无法用字节偏移提取，需先转为 RGB24 再比较。
 */
static bool isDirectlyExtractableRGB(AVPixelFormat format) {
    switch (format) {
        case AV_PIX_FMT_RGB24:
        case AV_PIX_FMT_BGR24:
        case AV_PIX_FMT_ARGB:
        case AV_PIX_FMT_RGBA:
        case AV_PIX_FMT_ABGR:
        case AV_PIX_FMT_BGRA:
        case AV_PIX_FMT_RGB0:
        case AV_PIX_FMT_BGR0:
        case AV_PIX_FMT_0RGB:
        case AV_PIX_FMT_0BGR:
        case AV_PIX_FMT_GBRP:
            return true;
        default:
            return false;
    }
}

FrameCompareResult BufferComparator::compareAuto(
    const ImageMeta& ref_img, const FormatInfo& ref_info,
    const ImageMeta& test_img, const FormatInfo& test_info
) {
    // 情况1：格式完全一致 → 直接对比（最快）
    if (ref_info.format == test_info.format) {
        if (config_.verbose && compare_count_.load() == 1) {
            LOG_DEBUG("[BufferComparator] Strategy: SAME_FORMAT (fastest)");
        }
        
        if (ref_info.is_yuv) {
            return compareYUV(ref_img, ref_info, test_img, test_info);
        } else if (ref_info.is_rgb) {
            // 同格式但可能是子字节格式（如两个 RGB565）
            if (!ref_info.is_mat && !isDirectlyExtractableRGB(ref_info.format)) {
                return compareSubByteRGB(ref_img, ref_info, test_img, test_info);
            }
            return compareRGB(ref_img, ref_info, test_img, test_info);
        }
    }
    
    // 情况2：都是YUV家族 → YUV空间对比
    if (ref_info.is_yuv && test_info.is_yuv) {
        if (config_.verbose && compare_count_.load() == 1) {
            LOG_DEBUG("[BufferComparator] Strategy: YUV_FAMILY");
            if (ref_info.format != test_info.format) {
                LOG_WARN("  YUV formats differ, will convert if needed");
            }
        }

        // packed 422（yuyv/uyvy）与 NV12/YUV420P 等混用时，不能直接 compareYUV
        if (ref_info.format != test_info.format) {
            if (config_.verbose && compare_count_.load() == 1) {
                LOG_DEBUG("[BufferComparator] Strategy: YUV_FAMILY → compareMixed (YUV420P)");
            }
            return compareMixed(ref_img, ref_info, test_img, test_info);
        }

        return compareYUV(ref_img, ref_info, test_img, test_info);
    }
    
    // 情况3：都是RGB家族 → RGB空间对比
    if (ref_info.is_rgb && test_info.is_rgb) {
        // 子字节 packed RGB（如 RGB444/555/565）无法直接按字节提取通道
        bool ref_extractable = ref_info.is_mat || isDirectlyExtractableRGB(ref_info.format);
        bool test_extractable = test_info.is_mat || isDirectlyExtractableRGB(test_info.format);
        
        if (ref_extractable && test_extractable) {
            if (config_.verbose && compare_count_.load() == 1) {
                LOG_DEBUG("[BufferComparator] Strategy: RGB_FAMILY (direct)");
            }
            return compareRGB(ref_img, ref_info, test_img, test_info);
        }
        
        if (config_.verbose && compare_count_.load() == 1) {
            LOG_WARN("[BufferComparator] Strategy: RGB_FAMILY → convert to RGB24 (sub-byte packed format)");
        }
        return compareSubByteRGB(ref_img, ref_info, test_img, test_info);
    }
    
    // 情况4：YUV vs RGB → 转换到YUV空间对比（业界标准）
    if ((ref_info.is_yuv && test_info.is_rgb) || (ref_info.is_rgb && test_info.is_yuv)) {
        if (config_.verbose && compare_count_.load() == 1) {
            LOG_WARN("[BufferComparator] Strategy: MIXED_FORMAT (YUV vs RGB)");
            LOG_WARN("  Converting to YUV420P for comparison (industry standard)");
        }
        
        return compareMixed(ref_img, ref_info, test_img, test_info);
    }
    
    // 未知格式
    FrameCompareResult result;
    result.error_message = "Unsupported format combination";
    result.passed = false;
    result.level = FrameCompareResult::FAIL;
    return result;
}

// ============================================================================
// YUV格式对比
// ============================================================================

FrameCompareResult BufferComparator::compareYUV(
    const ImageMeta& ref_img, const FormatInfo& ref_info,
    const ImageMeta& test_img, const FormatInfo& test_info
) {
    FrameCompareResult result;
    
    // ============================================================================
    // 层1：快速验证（仅Y平面）
    // ============================================================================
    
    bool quick_pass = false;
    
    // 🚀 并行计算 PSNR-Y 和 SSIM-Y（如果都启用）
    if (config_.enable_parallel && config_.enable_psnr && config_.enable_ssim) {
        // 方案B：完全并行 - 使用全局线程池
        auto& pool = GlobalThreadPool::getInstance().getThreadPool();
        
        // 提交两个异步任务
        auto future_psnr = pool.submit_task([this, &ref_img, &test_img, &ref_info, &test_info]() {
            return this->calculatePSNR_YUV_Y(ref_img, test_img, ref_info, test_info);
        });
        
        auto future_ssim = pool.submit_task([this, &ref_img, &test_img, &ref_info, &test_info]() {
            return this->calculateSSIM_YUV_Y(ref_img, test_img, ref_info, test_info);
        });
        
        // 等待结果
        result.psnr_y = future_psnr.get();
        result.ssim_y = future_ssim.get();
        
        // 两者都要达标
        quick_pass = (result.psnr_y >= config_.min_psnr) && 
                     (result.ssim_y >= config_.min_ssim);
    } 
    else {
        // 串行计算（兼容模式或只启用一个指标）
        if (config_.enable_psnr) {
            result.psnr_y = calculatePSNR_YUV_Y(ref_img, test_img, ref_info, test_info);
        }
        
        if (config_.enable_ssim) {
            result.ssim_y = calculateSSIM_YUV_Y(ref_img, test_img, ref_info, test_info);
        }
        
        // 判定快速通过条件（严格模式：都要满足）
        if (config_.enable_psnr && config_.enable_ssim) {
            quick_pass = (result.psnr_y >= config_.min_psnr) && 
                         (result.ssim_y >= config_.min_ssim);
        } else if (config_.enable_psnr) {
            quick_pass = (result.psnr_y >= config_.min_psnr);
        } else if (config_.enable_ssim) {
            quick_pass = (result.ssim_y >= config_.min_ssim);
        }
    }
    
    if (quick_pass) {
        // ✅ 快速通过（日志在 compare() 返回前统一打印）
        result.passed = true;
        result.level = FrameCompareResult::PASS;
        // 快速通过时，使用 Y 平面值作为平均值（因为未计算 U/V）
        result.psnr_avg = result.psnr_y;
        result.ssim_avg = result.ssim_y;
        passed_count_++;
        return result;
    }

    // 未快速通过：先用 Y 分量填充 avg，保证 ENC_COMPARE 等能读到真实数值；
    // FAST_ONLY 策略不会进入层2，此前此处未赋值会导致 psnr_avg/ssim_avg 恒为 0。
    // AUTO_LAYERED / DEEP_ALWAYS 下层2 会用 YUV 加权结果覆盖。
    if (config_.enable_psnr) {
        result.psnr_avg = result.psnr_y;
    }
    if (config_.enable_ssim) {
        result.ssim_avg = result.ssim_y;
    }
    
    // ============================================================================
    // 层2：深度验证（U/V平面）
    // ============================================================================
    
    if (config_.strategy == CompareConfig::AUTO_LAYERED ||
        config_.strategy == CompareConfig::DEEP_ALWAYS) {
        
        // 日志在 compare() 返回前统一打印
        
        // 🚀 方案B：完全并行 - 并行计算所有 U/V 平面（最多4个任务）
        if (config_.enable_parallel && config_.enable_psnr && config_.enable_ssim) {
            // 完全并行：4 个任务（PSNR-U, PSNR-V, SSIM-U, SSIM-V）
            auto& pool = GlobalThreadPool::getInstance().getThreadPool();
            
            auto future_psnr_u = pool.submit_task([this, &ref_img, &test_img, &ref_info, &test_info]() {
                return this->calculatePSNR_YUV_U(ref_img, test_img, ref_info, test_info);
            });
            
            auto future_psnr_v = pool.submit_task([this, &ref_img, &test_img, &ref_info, &test_info]() {
                return this->calculatePSNR_YUV_V(ref_img, test_img, ref_info, test_info);
            });
            
            auto future_ssim_u = pool.submit_task([this, &ref_img, &test_img, &ref_info, &test_info]() {
                return this->calculateSSIM_YUV_U(ref_img, test_img, ref_info, test_info);
            });
            
            auto future_ssim_v = pool.submit_task([this, &ref_img, &test_img, &ref_info, &test_info]() {
                return this->calculateSSIM_YUV_V(ref_img, test_img, ref_info, test_info);
            });
            
            // 等待所有结果
            result.psnr_u = future_psnr_u.get();
            result.psnr_v = future_psnr_v.get();
            result.ssim_u = future_ssim_u.get();
            result.ssim_v = future_ssim_v.get();
            
            // 加权平均
            if (config_.use_perceptual_weighting) {
                result.psnr_avg = (result.psnr_y * 4.0 + result.psnr_u + result.psnr_v) / 6.0;
                result.ssim_avg = (result.ssim_y * 4.0 + result.ssim_u + result.ssim_v) / 6.0;
            } else {
                result.psnr_avg = (result.psnr_y + result.psnr_u + result.psnr_v) / 3.0;
                result.ssim_avg = (result.ssim_y + result.ssim_u + result.ssim_v) / 3.0;
            }
        }
        else if (config_.enable_parallel && (config_.enable_psnr || config_.enable_ssim)) {
            // 部分并行：2 个任务（PSNR-UV 或 SSIM-UV）
            auto& pool = GlobalThreadPool::getInstance().getThreadPool();
            
            if (config_.enable_psnr) {
                auto future_psnr_u = pool.submit_task([this, &ref_img, &test_img, &ref_info, &test_info]() {
                    return this->calculatePSNR_YUV_U(ref_img, test_img, ref_info, test_info);
                });
                
                auto future_psnr_v = pool.submit_task([this, &ref_img, &test_img, &ref_info, &test_info]() {
                    return this->calculatePSNR_YUV_V(ref_img, test_img, ref_info, test_info);
                });
                
                result.psnr_u = future_psnr_u.get();
                result.psnr_v = future_psnr_v.get();
                
                // 加权平均
                if (config_.use_perceptual_weighting) {
                    result.psnr_avg = (result.psnr_y * 4.0 + result.psnr_u + result.psnr_v) / 6.0;
                } else {
                    result.psnr_avg = (result.psnr_y + result.psnr_u + result.psnr_v) / 3.0;
                }
            }
            
            if (config_.enable_ssim) {
                auto future_ssim_u = pool.submit_task([this, &ref_img, &test_img, &ref_info, &test_info]() {
                    return this->calculateSSIM_YUV_U(ref_img, test_img, ref_info, test_info);
                });
                
                auto future_ssim_v = pool.submit_task([this, &ref_img, &test_img, &ref_info, &test_info]() {
                    return this->calculateSSIM_YUV_V(ref_img, test_img, ref_info, test_info);
                });
                
                result.ssim_u = future_ssim_u.get();
                result.ssim_v = future_ssim_v.get();
                
                // 加权平均
                if (config_.use_perceptual_weighting) {
                    result.ssim_avg = (result.ssim_y * 4.0 + result.ssim_u + result.ssim_v) / 6.0;
                } else {
                    result.ssim_avg = (result.ssim_y + result.ssim_u + result.ssim_v) / 3.0;
                }
            }
        }
        else {
            // 串行计算（兼容模式）
            if (config_.enable_psnr) {
                result.psnr_u = calculatePSNR_YUV_U(ref_img, test_img, ref_info, test_info);
                result.psnr_v = calculatePSNR_YUV_V(ref_img, test_img, ref_info, test_info);
                
                // 加权平均
                if (config_.use_perceptual_weighting) {
                    result.psnr_avg = (result.psnr_y * 4.0 + result.psnr_u + result.psnr_v) / 6.0;
                } else {
                    result.psnr_avg = (result.psnr_y + result.psnr_u + result.psnr_v) / 3.0;
                }
            }
            
            if (config_.enable_ssim) {
                result.ssim_u = calculateSSIM_YUV_U(ref_img, test_img, ref_info, test_info);
                result.ssim_v = calculateSSIM_YUV_V(ref_img, test_img, ref_info, test_info);
                
                // 加权平均
                if (config_.use_perceptual_weighting) {
                    result.ssim_avg = (result.ssim_y * 4.0 + result.ssim_u + result.ssim_v) / 6.0;
                } else {
                    result.ssim_avg = (result.ssim_y + result.ssim_u + result.ssim_v) / 3.0;
                }
            }
        }
        
        // 判定（严格模式：都要满足）
        bool is_warn = false;
        
        if (config_.enable_psnr && config_.enable_ssim) {
            // 两者都启用：都要满足警告阈值
            is_warn = (result.psnr_y >= config_.warn_psnr) && 
                      (result.ssim_y >= config_.warn_ssim);
        } else if (config_.enable_psnr) {
            is_warn = (result.psnr_y >= config_.warn_psnr);
        } else if (config_.enable_ssim) {
            is_warn = (result.ssim_y >= config_.warn_ssim);
        }
        
        if (is_warn) {
            result.level = FrameCompareResult::WARN;
            result.passed = true;
            warned_count_++;
            // 日志在 compare() 返回前统一打印
        } else {
            result.level = FrameCompareResult::FAIL;
            result.passed = false;
            failed_count_++;
            // 日志在 compare() 返回前统一打印
        }
    } else {
        // FAST_ONLY 模式（avg 已在层1 后设为 Y 分量）
        result.level = FrameCompareResult::FAIL;
        result.passed = false;
        failed_count_++;
    }
    
    return result;
}

// ============================================================================
// RGB格式对比
// ============================================================================

FrameCompareResult BufferComparator::compareRGB(
    const ImageMeta& ref_img, const FormatInfo& ref_info,
    const ImageMeta& test_img, const FormatInfo& test_info
) {
    FrameCompareResult result;
    
    // ============================================================================
    // 层1：快速验证（G通道，人眼最敏感）
    // ============================================================================
    
    bool quick_pass = false;
    
    // 🚀 并行计算 PSNR-G 和 SSIM-G（如果都启用）
    if (config_.enable_parallel && config_.enable_psnr && config_.enable_ssim) {
        // 方案B：完全并行 - 使用全局线程池
        auto& pool = GlobalThreadPool::getInstance().getThreadPool();
        
        auto future_psnr = pool.submit_task([this, &ref_img, &test_img, &ref_info, &test_info]() {
            return this->calculatePSNR_RGB_G(ref_img, test_img, ref_info, test_info);
        });
        
        auto future_ssim = pool.submit_task([this, &ref_img, &test_img, &ref_info, &test_info]() {
            return this->calculateSSIM_RGB_G(ref_img, test_img, ref_info, test_info);
        });
        
        // 等待结果
        result.psnr_y = future_psnr.get();  // G → Y
        result.ssim_y = future_ssim.get();  // G → Y
        
        // 两者都要达标
        quick_pass = (result.psnr_y >= config_.min_psnr) && 
                     (result.ssim_y >= config_.min_ssim);
    }
    else {
        // 串行计算
        if (config_.enable_psnr) {
            double psnr_g = calculatePSNR_RGB_G(ref_img, test_img, ref_info, test_info);
            result.psnr_y = psnr_g;  // 复用psnr_y字段
        }
        
        if (config_.enable_ssim) {
            double ssim_g = calculateSSIM_RGB_G(ref_img, test_img, ref_info, test_info);
            result.ssim_y = ssim_g;  // 复用ssim_y字段
        }
        
        // 判定快速通过条件（严格模式：都要满足）
        if (config_.enable_psnr && config_.enable_ssim) {
            quick_pass = (result.psnr_y >= config_.min_psnr) && 
                         (result.ssim_y >= config_.min_ssim);
        } else if (config_.enable_psnr) {
            quick_pass = (result.psnr_y >= config_.min_psnr);
        } else if (config_.enable_ssim) {
            quick_pass = (result.ssim_y >= config_.min_ssim);
        }
    }
    
    if (quick_pass) {
        // ✅ 快速通过（日志在 compare() 返回前统一打印）
        result.passed = true;
        result.level = FrameCompareResult::PASS;
        // 快速通过时，使用 G 通道值作为平均值（因为未计算 R/B）
        result.psnr_avg = result.psnr_y;  // G → psnr_y
        result.ssim_avg = result.ssim_y;  // G → ssim_y
        passed_count_++;
        return result;
    }

    if (config_.enable_psnr) {
        result.psnr_avg = result.psnr_y;
    }
    if (config_.enable_ssim) {
        result.ssim_avg = result.ssim_y;
    }
    
    // ============================================================================
    // 层2：深度验证（R/B通道）
    // ============================================================================
    
    if (config_.strategy == CompareConfig::AUTO_LAYERED ||
        config_.strategy == CompareConfig::DEEP_ALWAYS) {
        
        // 日志在 compare() 返回前统一打印
        if (false) {  // 保留代码结构，但不执行日志打印
            if (config_.enable_psnr && config_.enable_ssim) {
                LOG_WARN_FMT("  Frame %d: PSNR-G=%.2f dB, SSIM-G=%.4f, deep validation...",
                             result.frame_index, result.psnr_y, result.ssim_y);
            } else if (config_.enable_psnr) {
                LOG_WARN_FMT("  Frame %d: PSNR-G=%.2f dB < %.2f dB, deep validation...",
                             result.frame_index, result.psnr_y, config_.min_psnr);
            } else {
                LOG_WARN_FMT("  Frame %d: SSIM-G=%.4f < %.4f, deep validation...",
                             result.frame_index, result.ssim_y, config_.min_ssim);
            }
        }
        
        double psnr_g = result.psnr_y;  // 保存已计算的 G 通道值
        double ssim_g = result.ssim_y;
        
        // 🚀 方案B：完全并行 - 并行计算 R/B 通道（最多4个任务）
        if (config_.enable_parallel && config_.enable_psnr && config_.enable_ssim) {
            // 完全并行：4 个任务（PSNR-R, PSNR-B, SSIM-R, SSIM-B）
            auto& pool = GlobalThreadPool::getInstance().getThreadPool();
            
            auto future_psnr_r = pool.submit_task([this, &ref_img, &test_img, &ref_info, &test_info]() {
                return this->calculatePSNR_RGB_R(ref_img, test_img, ref_info, test_info);
            });
            
            auto future_psnr_b = pool.submit_task([this, &ref_img, &test_img, &ref_info, &test_info]() {
                return this->calculatePSNR_RGB_B(ref_img, test_img, ref_info, test_info);
            });
            
            auto future_ssim_r = pool.submit_task([this, &ref_img, &test_img, &ref_info, &test_info]() {
                return this->calculateSSIM_RGB_R(ref_img, test_img, ref_info, test_info);
            });
            
            auto future_ssim_b = pool.submit_task([this, &ref_img, &test_img, &ref_info, &test_info]() {
                return this->calculateSSIM_RGB_B(ref_img, test_img, ref_info, test_info);
            });
            
            // 等待所有结果
            double psnr_r = future_psnr_r.get();
            double psnr_b = future_psnr_b.get();
            double ssim_r = future_ssim_r.get();
            double ssim_b = future_ssim_b.get();
            
            // 加权平均（G通道权重更高）
            if (config_.use_perceptual_weighting) {
                result.psnr_avg = (psnr_r + psnr_g * 2.0 + psnr_b) / 4.0;
                result.ssim_avg = (ssim_r + ssim_g * 2.0 + ssim_b) / 4.0;
            } else {
                result.psnr_avg = (psnr_r + psnr_g + psnr_b) / 3.0;
                result.ssim_avg = (ssim_r + ssim_g + ssim_b) / 3.0;
            }
            
            // 存储到结果（复用YUV字段）
            result.psnr_y = psnr_g;  // G → Y
            result.psnr_u = psnr_r;  // R → U
            result.psnr_v = psnr_b;  // B → V
            result.ssim_y = ssim_g;  // G → Y
            result.ssim_u = ssim_r;  // R → U
            result.ssim_v = ssim_b;  // B → V
        }
        else if (config_.enable_parallel && (config_.enable_psnr || config_.enable_ssim)) {
            // 部分并行：2 个任务
            auto& pool = GlobalThreadPool::getInstance().getThreadPool();
            
            if (config_.enable_psnr) {
                auto future_psnr_r = pool.submit_task([this, &ref_img, &test_img, &ref_info, &test_info]() {
                    return this->calculatePSNR_RGB_R(ref_img, test_img, ref_info, test_info);
                });
                
                auto future_psnr_b = pool.submit_task([this, &ref_img, &test_img, &ref_info, &test_info]() {
                    return this->calculatePSNR_RGB_B(ref_img, test_img, ref_info, test_info);
                });
                
                double psnr_r = future_psnr_r.get();
                double psnr_b = future_psnr_b.get();
                
                // 加权平均
                if (config_.use_perceptual_weighting) {
                    result.psnr_avg = (psnr_r + psnr_g * 2.0 + psnr_b) / 4.0;
                } else {
                    result.psnr_avg = (psnr_r + psnr_g + psnr_b) / 3.0;
                }
                
                // 存储到结果
                result.psnr_y = psnr_g;  // G → Y
                result.psnr_u = psnr_r;  // R → U
                result.psnr_v = psnr_b;  // B → V
            }
            
            if (config_.enable_ssim) {
                auto future_ssim_r = pool.submit_task([this, &ref_img, &test_img, &ref_info, &test_info]() {
                    return this->calculateSSIM_RGB_R(ref_img, test_img, ref_info, test_info);
                });
                
                auto future_ssim_b = pool.submit_task([this, &ref_img, &test_img, &ref_info, &test_info]() {
                    return this->calculateSSIM_RGB_B(ref_img, test_img, ref_info, test_info);
                });
                
                double ssim_r = future_ssim_r.get();
                double ssim_b = future_ssim_b.get();
                
                // 加权平均
                if (config_.use_perceptual_weighting) {
                    result.ssim_avg = (ssim_r + ssim_g * 2.0 + ssim_b) / 4.0;
                } else {
                    result.ssim_avg = (ssim_r + ssim_g + ssim_b) / 3.0;
                }
                
                // 存储到结果
                result.ssim_y = ssim_g;  // G → Y
                result.ssim_u = ssim_r;  // R → U
                result.ssim_v = ssim_b;  // B → V
            }
        }
        else {
            // 串行计算（兼容模式）
            if (config_.enable_psnr) {
                double psnr_r = calculatePSNR_RGB_R(ref_img, test_img, ref_info, test_info);
                double psnr_b = calculatePSNR_RGB_B(ref_img, test_img, ref_info, test_info);
                
                // 加权平均（G通道权重更高）
                if (config_.use_perceptual_weighting) {
                    result.psnr_avg = (psnr_r + psnr_g * 2.0 + psnr_b) / 4.0;
                } else {
                    result.psnr_avg = (psnr_r + psnr_g + psnr_b) / 3.0;
                }
                
                // 存储到结果（复用YUV字段）
                result.psnr_y = psnr_g;  // G → Y
                result.psnr_u = psnr_r;  // R → U
                result.psnr_v = psnr_b;  // B → V
            }
            
            if (config_.enable_ssim) {
                double ssim_r = calculateSSIM_RGB_R(ref_img, test_img, ref_info, test_info);
                double ssim_b = calculateSSIM_RGB_B(ref_img, test_img, ref_info, test_info);
                
                // 加权平均（G通道权重更高）
                if (config_.use_perceptual_weighting) {
                    result.ssim_avg = (ssim_r + ssim_g * 2.0 + ssim_b) / 4.0;
                } else {
                    result.ssim_avg = (ssim_r + ssim_g + ssim_b) / 3.0;
                }
                
                // 存储到结果（复用YUV字段）
                result.ssim_y = ssim_g;  // G → Y
                result.ssim_u = ssim_r;  // R → U
                result.ssim_v = ssim_b;  // B → V
            }
        }
        
        // 判定（严格模式：都要满足）
        bool is_warn = false;
        
        if (config_.enable_psnr && config_.enable_ssim) {
            is_warn = (result.psnr_y >= config_.warn_psnr) && 
                      (result.ssim_y >= config_.warn_ssim);
        } else if (config_.enable_psnr) {
            is_warn = (result.psnr_y >= config_.warn_psnr);
        } else if (config_.enable_ssim) {
            is_warn = (result.ssim_y >= config_.warn_ssim);
        }
        
        if (is_warn) {
            result.level = FrameCompareResult::WARN;
            result.passed = true;
            warned_count_++;
            // 日志在 compare() 返回前统一打印
        } else {
            result.level = FrameCompareResult::FAIL;
            result.passed = false;
            failed_count_++;
            // 日志在 compare() 返回前统一打印
        }
    } else {
        result.level = FrameCompareResult::FAIL;
        result.passed = false;
        failed_count_++;
    }
    
    return result;
}

// ============================================================================
// 混合格式对比（需要转换）
// ============================================================================

FrameCompareResult BufferComparator::compareMixed(
    const ImageMeta& ref_img, const FormatInfo& ref_info,
    const ImageMeta& test_img, const FormatInfo& test_info
) {
    FrameCompareResult result;
    
    // 情况1：ref是YUV，test是RGB → 将ref转换为RGB后对比
    if (ref_info.is_yuv && test_info.is_rgb) {
        if (config_.verbose) {
            LOG_DEBUG("[BufferComparator] Converting YUV (ref) to RGB for comparison");
        }
        
        // 将YUV转换为RGB（使用test的RGB格式）
        AVFrame* ref_rgb = convertYUVToRGB(ref_img, ref_info, test_info.format);
        
        if (!ref_rgb) {
            LOG_ERROR("[BufferComparator] Failed to convert YUV to RGB");
            result.error_message = "YUV to RGB conversion failed";
            result.passed = false;
            result.level = FrameCompareResult::FAIL;
            return result;
        }
        
        // 直接从转换后的 AVFrame 构建 ImageMeta，无需临时 Buffer
        ImageMeta ref_rgb_img = ImageMeta::fromAVFrame(ref_rgb);
        FormatInfo ref_rgb_info = analyzeFormat(ref_rgb_img);
        
        // 使用RGB对比函数进行对比
        // 子字节打包格式不能用 compareRGB 的字节偏移提取通道
        bool ref_extractable = ref_rgb_info.is_mat || isDirectlyExtractableRGB(ref_rgb_info.format);
        bool test_extractable = test_info.is_mat || isDirectlyExtractableRGB(test_info.format);
        if (ref_extractable && test_extractable) {
            result = compareRGB(ref_rgb_img, ref_rgb_info, test_img, test_info);
        } else {
            result = compareSubByteRGB(ref_rgb_img, ref_rgb_info, test_img, test_info);
        }
        
        freeConvertedFrame(ref_rgb);
        
        return result;
    }
    
    // 情况2：ref是RGB，test是YUV → 将test转换为RGB后对比
    if (ref_info.is_rgb && test_info.is_yuv) {
        if (config_.verbose) {
            LOG_DEBUG("[BufferComparator] Converting YUV (test) to RGB for comparison");
        }
        
        // 将YUV转换为RGB（使用ref的RGB格式）
        AVFrame* test_rgb = convertYUVToRGB(test_img, test_info, ref_info.format);
        
        if (!test_rgb) {
            LOG_ERROR("[BufferComparator] Failed to convert YUV to RGB");
            result.error_message = "YUV to RGB conversion failed";
            result.passed = false;
            result.level = FrameCompareResult::FAIL;
            return result;
        }
        
        // 直接从转换后的 AVFrame 构建 ImageMeta，无需临时 Buffer
        ImageMeta test_rgb_img = ImageMeta::fromAVFrame(test_rgb);
        FormatInfo test_rgb_info = analyzeFormat(test_rgb_img);
        
        // 使用RGB对比函数进行对比
        // 子字节打包格式（rgb444/555/565等）不能用 compareRGB 的字节偏移提取通道，
        // 需先 convertToRGB24 再比较
        bool ref_extractable = ref_info.is_mat || isDirectlyExtractableRGB(ref_info.format);
        bool test_extractable = test_rgb_info.is_mat || isDirectlyExtractableRGB(test_rgb_info.format);
        if (ref_extractable && test_extractable) {
            result = compareRGB(ref_img, ref_info, test_rgb_img, test_rgb_info);
        } else {
            result = compareSubByteRGB(ref_img, ref_info, test_rgb_img, test_rgb_info);
        }
        
        freeConvertedFrame(test_rgb);
        
        return result;
    }
    
    // 其他情况：都转换为YUV420P（用于其他混合格式对比）
    AVFrame* ref_yuv = convertToYUV420P(ref_img, ref_info);
    AVFrame* test_yuv = convertToYUV420P(test_img, test_info);
    
    if (!ref_yuv || !test_yuv) {
        LOG_ERROR("[BufferComparator] Format conversion failed");
        
        if (ref_yuv) freeConvertedFrame(ref_yuv);
        if (test_yuv) freeConvertedFrame(test_yuv);
        
        result.error_message = "Format conversion failed";
        result.passed = false;
        result.level = FrameCompareResult::FAIL;
        return result;
    }
    
    // 直接从转换后的 AVFrame 构建 ImageMeta，无需临时 Buffer
    ImageMeta ref_yuv_img = ImageMeta::fromAVFrame(ref_yuv);
    ImageMeta test_yuv_img = ImageMeta::fromAVFrame(test_yuv);
    FormatInfo ref_yuv_info = analyzeFormat(ref_yuv_img);
    FormatInfo test_yuv_info = analyzeFormat(test_yuv_img);
    
    // 使用YUV对比函数进行对比
    result = compareYUV(ref_yuv_img, ref_yuv_info, test_yuv_img, test_yuv_info);
    
    freeConvertedFrame(ref_yuv);
    freeConvertedFrame(test_yuv);
    
    return result;
}

// ============================================================================
// 子字节 packed RGB 格式对比（RGB444/555/565 等）
// ============================================================================

AVFrame* BufferComparator::convertToRGB24(const ImageMeta& img, const FormatInfo& info) {
    if (!img.isValid() || info.format == AV_PIX_FMT_NONE) {
        return nullptr;
    }
    
    // 已经是 RGB24 则直接 clone
    if (info.format == AV_PIX_FMT_RGB24) {
        AVFrame* tmp = av_frame_alloc();
        if (!tmp) return nullptr;
        tmp->format = AV_PIX_FMT_RGB24;
        tmp->width = info.width;
        tmp->height = info.height;
        for (int i = 0; i < img.nbPlanes(); i++) {
            tmp->data[i] = img.planeData(i);
            tmp->linesize[i] = img.linesize(i);
        }
        AVFrame* cloned = av_frame_clone(tmp);
        av_frame_free(&tmp);
        return cloned;
    }
    
    AVFrame* dst = av_frame_alloc();
    if (!dst) return nullptr;
    
    dst->format = AV_PIX_FMT_RGB24;
    dst->width = info.width;
    dst->height = info.height;
    
    if (av_frame_get_buffer(dst, 0) < 0) {
        av_frame_free(&dst);
        return nullptr;
    }
    
    const uint8_t* src_data[4] = {
        img.planeData(0), img.planeData(1), img.planeData(2), img.planeData(3)
    };
    int src_linesize[4] = {
        img.linesize(0), img.linesize(1), img.linesize(2), img.linesize(3)
    };
    
    SwsContext* sws = sws_getContext(
        info.width, info.height, info.format,
        info.width, info.height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    
    if (!sws) {
        av_frame_free(&dst);
        return nullptr;
    }
    
    sws_scale(sws, src_data, src_linesize, 0, info.height,
              dst->data, dst->linesize);
    sws_freeContext(sws);
    
    return dst;
}

FrameCompareResult BufferComparator::compareSubByteRGB(
    const ImageMeta& ref_img, const FormatInfo& ref_info,
    const ImageMeta& test_img, const FormatInfo& test_info
) {
    if (config_.verbose) {
        LOG_WARN_FMT("[BufferComparator] Sub-byte RGB: converting %s + %s → RGB24",
                     ref_info.name.c_str(), test_info.name.c_str());
    }
    
    AVFrame* ref_rgb24 = convertToRGB24(ref_img, ref_info);
    AVFrame* test_rgb24 = convertToRGB24(test_img, test_info);
    
    if (!ref_rgb24 || !test_rgb24) {
        if (ref_rgb24) freeConvertedFrame(ref_rgb24);
        if (test_rgb24) freeConvertedFrame(test_rgb24);
        
        FrameCompareResult result;
        result.error_message = "Sub-byte RGB to RGB24 conversion failed";
        result.passed = false;
        result.level = FrameCompareResult::FAIL;
        return result;
    }
    
    ImageMeta ref_rgb24_img = ImageMeta::fromAVFrame(ref_rgb24);
    ImageMeta test_rgb24_img = ImageMeta::fromAVFrame(test_rgb24);
    FormatInfo ref_rgb24_info = analyzeFormat(ref_rgb24_img);
    FormatInfo test_rgb24_info = analyzeFormat(test_rgb24_img);
    
    FrameCompareResult result = compareRGB(ref_rgb24_img, ref_rgb24_info,
                                           test_rgb24_img, test_rgb24_info);
    
    freeConvertedFrame(ref_rgb24);
    freeConvertedFrame(test_rgb24);
    
    return result;
}

// ============================================================================
// PSNR计算 - 通用方法
// ============================================================================

double BufferComparator::calculatePSNR(
    const uint8_t* data1, const uint8_t* data2,
    int width, int height,
    int stride1, int stride2
) {
    if (!data1 || !data2 || width <= 0 || height <= 0) {
        return 0.0;
    }
    
    uint64_t mse = 0;
    
    // 逐行对比（考虑stride）
    for (int y = 0; y < height; y++) {
        const uint8_t* row1 = data1 + y * stride1;
        const uint8_t* row2 = data2 + y * stride2;
        
        for (int x = 0; x < width; x++) {
            int diff = (int)row1[x] - (int)row2[x];
            mse += (uint64_t)(diff * diff);
        }
    }
    
    if (mse == 0) {
        return 100.0;  // 完全一致
    }
    
    double mean_mse = (double)mse / (width * height);
    double psnr = 10.0 * log10((255.0 * 255.0) / mean_mse);
    
    return psnr;
}

// ============================================================================
// PSNR计算 - YUV格式
// ============================================================================

double BufferComparator::calculatePSNR_YUV_Y(
    const ImageMeta& img1, const ImageMeta& img2,
    const FormatInfo& info1, const FormatInfo& info2
) {
    // ImageMeta 统一了 Mat 和 AVFrame 路径：plane_data[0] 和 linesize[0] 已就绪
    uint8_t* data1 = img1.planeData(0);
    uint8_t* data2 = img2.planeData(0);
    
    if (!data1 || !data2) {
        return 0.0;
    }
    
    return calculatePSNR(data1, data2, info1.width, info1.height,
                        img1.linesize(0), img2.linesize(0));
}

double BufferComparator::calculatePSNR_YUV_U(
    const ImageMeta& img1, const ImageMeta& img2,
    const FormatInfo& info1, const FormatInfo& info2
) {
    // Mat 单通道没有 U 平面，视为完全一致
    if (info1.is_mat || info2.is_mat) {
        return 100.0;
    }

    // 获取U平面数据
    uint8_t* data1 = img1.planeData(1);
    uint8_t* data2 = img2.planeData(1);
    
    if (!data1 || !data2) {
        return 100.0;  // 无U平面，认为一致
    }
    
    // U平面通常是原始分辨率的一半
    int uv_width = info1.width / 2;
    int uv_height = info1.height / 2;
    
    return calculatePSNR(data1, data2, uv_width, uv_height,
                        img1.linesize(1), img2.linesize(1));
}

double BufferComparator::calculatePSNR_YUV_V(
    const ImageMeta& img1, const ImageMeta& img2,
    const FormatInfo& info1, const FormatInfo& info2
) {
    // Mat 单通道没有 V 平面，视为完全一致
    if (info1.is_mat || info2.is_mat) {
        return 100.0;
    }

    // 获取V平面数据
    uint8_t* data1 = img1.planeData(2);
    uint8_t* data2 = img2.planeData(2);
    
    if (!data1 || !data2) {
        return 100.0;  // 无V平面，认为一致
    }
    
    int uv_width = info1.width / 2;
    int uv_height = info1.height / 2;
    
    return calculatePSNR(data1, data2, uv_width, uv_height,
                        img1.linesize(2), img2.linesize(2));
}

// ============================================================================
// PSNR计算 - RGB格式
// ============================================================================

/**
 * @brief 提取RGB通道到临时缓冲区（用于通道分离计算）
 * @param img ImageMeta 引用
 * @param format 像素格式
 * @param width 宽度
 * @param height 高度
 * @param channel 通道索引：0=R, 1=G, 2=B
 * @param out_data 输出缓冲区（需要预先分配 width*height 字节）
 * @return 成功返回true
 */
static bool extractRGBChannel(
    const ImageMeta& img, AVPixelFormat format, int width, int height,
    int channel, uint8_t* out_data
) {
    if (!out_data || channel < 0 || channel > 2) {
        return false;
    }

    uint8_t* src_data = img.planeData(0);
    if (!src_data) {
        return false;
    }
    
    int src_stride = img.linesize(0);
    
    // Mat packed 路径：format == NONE 表示来自 fromMat()
    if (format == AV_PIX_FMT_NONE && src_data) {
        // 通道数通过 linesize / width 推算
        int channels = (width > 0) ? src_stride / width : 1;
        if (channel >= channels) {
            return false;
        }
        // 单通道退化情况
        if (channels == 1) {
            for (int y = 0; y < height; y++) {
                memcpy(out_data + y * width, src_data + y * src_stride, width);
            }
            return true;
        }
        // 多通道：逐像素提取指定通道
        for (int y = 0; y < height; y++) {
            const uint8_t* src_row = src_data + y * src_stride;
            uint8_t* dst_row = out_data + y * width;
            for (int x = 0; x < width; x++) {
                dst_row[x] = src_row[x * channels + channel];
            }
        }
        return true;
    }
    
    // Planar格式（GBRP）：直接使用对应plane
    if (format == AV_PIX_FMT_GBRP) {
        uint8_t* plane_data = img.planeData(channel);
        if (!plane_data) return false;
        int plane_stride = img.linesize(channel);
        
        for (int y = 0; y < height; y++) {
            memcpy(out_data + y * width, plane_data + y * plane_stride, width);
        }
        return true;
    }
    
    // Packed格式：需要从打包数据中提取
    int bytes_per_pixel = 0;
    int r_offset = -1, g_offset = -1, b_offset = -1;
    
    switch (format) {
        case AV_PIX_FMT_RGB24:
            bytes_per_pixel = 3;
            r_offset = 0; g_offset = 1; b_offset = 2;
            break;
        case AV_PIX_FMT_BGR24:
            bytes_per_pixel = 3;
            r_offset = 2; g_offset = 1; b_offset = 0;
            break;
        case AV_PIX_FMT_ARGB:
            bytes_per_pixel = 4;
            r_offset = 1; g_offset = 2; b_offset = 3;
            break;
        case AV_PIX_FMT_RGBA:
            bytes_per_pixel = 4;
            r_offset = 0; g_offset = 1; b_offset = 2;
            break;
        case AV_PIX_FMT_ABGR:
            bytes_per_pixel = 4;
            r_offset = 3; g_offset = 2; b_offset = 1;
            break;
        case AV_PIX_FMT_BGRA:
            bytes_per_pixel = 4;
            r_offset = 2; g_offset = 1; b_offset = 0;
            break;
        case AV_PIX_FMT_RGB0:
            bytes_per_pixel = 4;
            r_offset = 0; g_offset = 1; b_offset = 2;
            break;
        case AV_PIX_FMT_BGR0:
            bytes_per_pixel = 4;
            r_offset = 2; g_offset = 1; b_offset = 0;
            break;
        case AV_PIX_FMT_0RGB:
            bytes_per_pixel = 4;
            r_offset = 1; g_offset = 2; b_offset = 3;
            break;
        case AV_PIX_FMT_0BGR:
            bytes_per_pixel = 4;
            r_offset = 3; g_offset = 2; b_offset = 1;
            break;
        default:
            return false;  // 不支持的格式
    }
    
    int channel_offset = (channel == 0) ? r_offset : (channel == 1) ? g_offset : b_offset;
    if (channel_offset < 0) return false;
    
    // 提取通道数据
    for (int y = 0; y < height; y++) {
        const uint8_t* src_row = src_data + y * src_stride;
        uint8_t* dst_row = out_data + y * width;
        
        for (int x = 0; x < width; x++) {
            dst_row[x] = src_row[x * bytes_per_pixel + channel_offset];
        }
    }
    
    return true;
}

// ============================================================================
// PSNR计算 - RGB格式
// ============================================================================

double BufferComparator::calculatePSNR_RGB_R(
    const ImageMeta& img1, const ImageMeta& img2,
    const FormatInfo& info1, const FormatInfo& info2
) {
    if (!img1.isValid() || !img2.isValid()) return 0.0;
    
    // 分配临时缓冲区
    std::vector<uint8_t> channel1(info1.width * info1.height);
    std::vector<uint8_t> channel2(info2.width * info2.height);
    
    if (!extractRGBChannel(img1, info1.format, info1.width, info1.height, 0, channel1.data()) ||
        !extractRGBChannel(img2, info2.format, info2.width, info2.height, 0, channel2.data())) {
        return 0.0;
    }
    
    // 计算R通道PSNR
    return calculatePSNR(channel1.data(), channel2.data(), 
                        info1.width, info1.height,
                        info1.width, info2.width);
}

double BufferComparator::calculatePSNR_RGB_G(
    const ImageMeta& img1, const ImageMeta& img2,
    const FormatInfo& info1, const FormatInfo& info2
) {
    if (!img1.isValid() || !img2.isValid()) return 0.0;
    
    std::vector<uint8_t> channel1(info1.width * info1.height);
    std::vector<uint8_t> channel2(info2.width * info2.height);
    
    if (!extractRGBChannel(img1, info1.format, info1.width, info1.height, 1, channel1.data()) ||
        !extractRGBChannel(img2, info2.format, info2.width, info2.height, 1, channel2.data())) {
        return 0.0;
    }
    
    return calculatePSNR(channel1.data(), channel2.data(), 
                        info1.width, info1.height,
                        info1.width, info2.width);
}

double BufferComparator::calculatePSNR_RGB_B(
    const ImageMeta& img1, const ImageMeta& img2,
    const FormatInfo& info1, const FormatInfo& info2
) {
    if (!img1.isValid() || !img2.isValid()) return 0.0;
    
    std::vector<uint8_t> channel1(info1.width * info1.height);
    std::vector<uint8_t> channel2(info2.width * info2.height);
    
    if (!extractRGBChannel(img1, info1.format, info1.width, info1.height, 2, channel1.data()) ||
        !extractRGBChannel(img2, info2.format, info2.width, info2.height, 2, channel2.data())) {
        return 0.0;
    }
    
    return calculatePSNR(channel1.data(), channel2.data(), 
                        info1.width, info1.height,
                        info1.width, info2.width);
}

// ============================================================================
// 格式转换
// ============================================================================

AVFrame* BufferComparator::convertToYUV420P(const ImageMeta& img, const FormatInfo& info) {
    if (!img.isValid()) {
        return nullptr;
    }
    
    // YUVJ420P/YUVJ422P/YUVJ444P 与对应的 YUV420P/422P/444P 像素布局完全相同，
    // 仅 color range 标记不同；PSNR/SSIM 计算无需 range 转换
    if (info.format == AV_PIX_FMT_YUV420P || info.format == AV_PIX_FMT_YUVJ420P) {
        // 从 ImageMeta 构造一个临时 AVFrame 并 clone
        AVFrame* tmp = av_frame_alloc();
        if (!tmp) return nullptr;
        tmp->format = info.format;
        tmp->width = info.width;
        tmp->height = info.height;
        for (int i = 0; i < img.nbPlanes(); i++) {
            tmp->data[i] = img.planeData(i);
            tmp->linesize[i] = img.linesize(i);
        }
        AVFrame* cloned = av_frame_clone(tmp);
        av_frame_free(&tmp);
        return cloned;
    }
    
    // NV12/NV21 是 semi-planar 420，Y 平面与 YUV420P 完全相同，
    // 仅 UV 平面从交织变为分离；直接 deinterleave 可避免
    // sws_scale 引入的 color range 偏移
    if (info.format == AV_PIX_FMT_NV12 || info.format == AV_PIX_FMT_NV21) {
        AVFrame* dst_frame = av_frame_alloc();
        if (!dst_frame) return nullptr;
        
        dst_frame->format = AV_PIX_FMT_YUV420P;
        dst_frame->width = info.width;
        dst_frame->height = info.height;
        
        if (av_frame_get_buffer(dst_frame, 0) < 0) {
            av_frame_free(&dst_frame);
            return nullptr;
        }
        
        int uv_width = info.width / 2;
        int uv_height = info.height / 2;
        
        // Y 平面：逐行复制（stride 可能不同）
        for (int y = 0; y < info.height; y++) {
            memcpy(dst_frame->data[0] + y * dst_frame->linesize[0],
                   img.planeData(0) + y * img.linesize(0),
                   info.width);
        }
        
        // UV 平面：从交织 (UVUVUV...) 拆分为独立 U 和 V 平面
        const uint8_t* uv_src = img.planeData(1);
        uint8_t* u_dst = dst_frame->data[1];
        uint8_t* v_dst = dst_frame->data[2];
        bool is_nv12 = (info.format == AV_PIX_FMT_NV12);
        
        for (int y = 0; y < uv_height; y++) {
            const uint8_t* uv_row = uv_src + y * img.linesize(1);
            uint8_t* u_row = u_dst + y * dst_frame->linesize[1];
            uint8_t* v_row = v_dst + y * dst_frame->linesize[2];
            
            for (int x = 0; x < uv_width; x++) {
                if (is_nv12) {
                    u_row[x] = uv_row[2 * x];
                    v_row[x] = uv_row[2 * x + 1];
                } else {
                    v_row[x] = uv_row[2 * x];
                    u_row[x] = uv_row[2 * x + 1];
                }
            }
        }
        
        return dst_frame;
    }
    
    // 创建目标frame
    AVFrame* dst_frame = av_frame_alloc();
    if (!dst_frame) {
        return nullptr;
    }
    
    dst_frame->format = AV_PIX_FMT_YUV420P;
    dst_frame->width = info.width;
    dst_frame->height = info.height;
    
    if (av_frame_get_buffer(dst_frame, 0) < 0) {
        av_frame_free(&dst_frame);
        return nullptr;
    }
    
    // 使用 ImageMeta 的 plane_data/linesize 构造 sws_scale 输入
    const uint8_t* src_data[4] = {
        img.planeData(0), img.planeData(1), img.planeData(2), img.planeData(3)
    };
    int src_linesize[4] = {
        img.linesize(0), img.linesize(1), img.linesize(2), img.linesize(3)
    };
    
    AVPixelFormat src_fmt = info.format;
    SwsContext* sws_ctx = sws_getContext(
        info.width, info.height, src_fmt,
        info.width, info.height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    
    if (!sws_ctx) {
        av_frame_free(&dst_frame);
        return nullptr;
    }
    
    sws_scale(sws_ctx, src_data, src_linesize, 0, info.height,
              dst_frame->data, dst_frame->linesize);
    
    sws_freeContext(sws_ctx);
    
    return dst_frame;
}

/**
 * @brief 将YUV格式转换为RGB格式
 * @param img YUV格式的ImageMeta
 * @param info YUV格式信息
 * @param target_rgb_format 目标RGB格式
 * @return 转换后的AVFrame，失败返回nullptr
 */
AVFrame* BufferComparator::convertYUVToRGB(
    const ImageMeta& img, const FormatInfo& info, AVPixelFormat target_rgb_format
) {
    if (!img.isValid()) {
        return nullptr;
    }
    
    // 创建目标frame
    AVFrame* dst_frame = av_frame_alloc();
    if (!dst_frame) {
        return nullptr;
    }
    
    dst_frame->format = target_rgb_format;
    dst_frame->width = info.width;
    dst_frame->height = info.height;
    
    if (av_frame_get_buffer(dst_frame, 0) < 0) {
        av_frame_free(&dst_frame);
        return nullptr;
    }
    
    // 使用 ImageMeta 的 plane_data/linesize 构造 sws_scale 输入
    const uint8_t* src_data[4] = {
        img.planeData(0), img.planeData(1), img.planeData(2), img.planeData(3)
    };
    int src_linesize[4] = {
        img.linesize(0), img.linesize(1), img.linesize(2), img.linesize(3)
    };
    
    // 创建转换上下文（YUV -> RGB）
    SwsContext* sws_ctx = sws_getContext(
        info.width, info.height, info.format,
        info.width, info.height, target_rgb_format,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    
    if (!sws_ctx) {
        av_frame_free(&dst_frame);
        return nullptr;
    }
    
    // 执行转换
    sws_scale(sws_ctx, src_data, src_linesize, 0, info.height,
              dst_frame->data, dst_frame->linesize);
    
    sws_freeContext(sws_ctx);
    
    return dst_frame;
}

void BufferComparator::freeConvertedFrame(AVFrame* frame) {
    if (frame) {
        av_frame_free(&frame);
    }
}

// ============================================================================
// 辅助方法
// ============================================================================

void BufferComparator::updateStatistics(const FrameCompareResult& result) {
    if (config_.enable_psnr && result.psnr_y > 0.0) {
        sum_psnr_y_ += result.psnr_y;
        sum_psnr_u_ += result.psnr_u;
        sum_psnr_v_ += result.psnr_v;
        
        if (result.psnr_y < min_psnr_y_) {
            min_psnr_y_ = result.psnr_y;
        }
        if (result.psnr_y > max_psnr_y_) {
            max_psnr_y_ = result.psnr_y;
        }
    }
    
    if (config_.enable_ssim && result.ssim_y > 0.0) {
        sum_ssim_y_ += result.ssim_y;
        sum_ssim_u_ += result.ssim_u;
        sum_ssim_v_ += result.ssim_v;
        
        if (result.ssim_y < min_ssim_y_) {
            min_ssim_y_ = result.ssim_y;
        }
        if (result.ssim_y > max_ssim_y_) {
            max_ssim_y_ = result.ssim_y;
        }
    }
    
    // 记录失败和警告帧
    if (result.level == FrameCompareResult::FAIL) {
        failures_.push_back(result);
    } else if (result.level == FrameCompareResult::WARN) {
        warnings_.push_back(result);
    }
}

void BufferComparator::writeReport(const FrameCompareResult& result) {
    if (!report_file_) {
        return;
    }
    
    // 只记录失败和警告帧
    if (result.level == FrameCompareResult::FAIL || 
        result.level == FrameCompareResult::WARN) {
        
        fprintf(report_file_, "Frame %d: %s\n", 
                result.frame_index,
                result.level == FrameCompareResult::FAIL ? "FAIL ❌" : "WARN ⚠️");
        fprintf(report_file_, "  Formats: %s vs %s\n",
                result.ref_format_name.c_str(),
                result.test_format_name.c_str());
        
        if (config_.enable_psnr) {
            fprintf(report_file_, "  PSNR: Y=%.2f U=%.2f V=%.2f dB (avg=%.2f dB)\n",
                    result.psnr_y, result.psnr_u, result.psnr_v, result.psnr_avg);
        }
        
        if (config_.enable_ssim) {
            fprintf(report_file_, "  SSIM: Y=%.4f U=%.4f V=%.4f (avg=%.4f)\n",
                    result.ssim_y, result.ssim_u, result.ssim_v, result.ssim_avg);
        }
        
        if (!result.error_message.empty()) {
            fprintf(report_file_, "  Error: %s\n", result.error_message.c_str());
        }
        
        fprintf(report_file_, "\n");
        fflush(report_file_);
    }
}

// ============================================================================
// SSIM计算 - 核心算法
// ============================================================================

/**
 * @brief SSIM 核心算法实现
 * 
 * SSIM = [亮度对比] × [对比度对比] × [结构对比]
 *      = l(x,y) × c(x,y) × s(x,y)
 * 
 * 其中：
 * - l(x,y) = (2*μx*μy + C1) / (μx² + μy² + C1)      亮度
 * - c(x,y) = (2*σx*σy + C2) / (σx² + σy² + C2)      对比度
 * - s(x,y) = (σxy + C3) / (σx*σy + C3)              结构
 * 
 * 简化公式（C3 = C2/2）：
 * SSIM = [(2*μx*μy + C1)(2*σxy + C2)] / [(μx² + μy² + C1)(σx² + σy² + C2)]
 */
double BufferComparator::calculateSSIM(
    const uint8_t* data1, const uint8_t* data2,
    int width, int height,
    int stride1, int stride2
) {
    if (!data1 || !data2 || width <= 0 || height <= 0) {
        return 0.0;
    }
    
    // SSIM 常量（ITU-T 标准）
    const double C1 = (0.01 * 255) * (0.01 * 255);  // (K1*L)^2, K1=0.01, L=255
    const double C2 = (0.03 * 255) * (0.03 * 255);  // (K2*L)^2, K2=0.03
    
    // 使用滑动窗口（8x8，简化版，标准是11x11高斯窗口）
    const int window_size = 8;
    const int step = 8;  // 步长=窗口大小，避免重叠（加速计算）
    
    double ssim_sum = 0.0;
    int window_count = 0;
    
    // 滑动窗口遍历
    for (int y = 0; y <= height - window_size; y += step) {
        for (int x = 0; x <= width - window_size; x += step) {
            // 计算窗口内的统计量
            double sum1 = 0.0, sum2 = 0.0;
            double sum1_sq = 0.0, sum2_sq = 0.0;
            double sum12 = 0.0;
            int n = window_size * window_size;
            
            for (int wy = 0; wy < window_size; wy++) {
                const uint8_t* row1 = data1 + (y + wy) * stride1 + x;
                const uint8_t* row2 = data2 + (y + wy) * stride2 + x;
                
                for (int wx = 0; wx < window_size; wx++) {
                    double p1 = row1[wx];
                    double p2 = row2[wx];
                    
                    sum1 += p1;
                    sum2 += p2;
                    sum1_sq += p1 * p1;
                    sum2_sq += p2 * p2;
                    sum12 += p1 * p2;
                }
            }
            
            // 计算均值和方差
            double mean1 = sum1 / n;
            double mean2 = sum2 / n;
            double variance1 = (sum1_sq / n) - (mean1 * mean1);
            double variance2 = (sum2_sq / n) - (mean2 * mean2);
            double covariance = (sum12 / n) - (mean1 * mean2);
            
            // 计算 SSIM
            double numerator = (2.0 * mean1 * mean2 + C1) * (2.0 * covariance + C2);
            double denominator = (mean1 * mean1 + mean2 * mean2 + C1) * 
                                (variance1 + variance2 + C2);
            
            double ssim = numerator / denominator;
            ssim_sum += ssim;
            window_count++;
        }
    }
    
    if (window_count == 0) {
        return 1.0;  // 窗口太小，认为相同
    }
    
    double mean_ssim = ssim_sum / window_count;
    
    // SSIM 范围应该在 [0, 1]，但实际计算可能略小于0（数值误差）
    if (mean_ssim < 0.0) mean_ssim = 0.0;
    if (mean_ssim > 1.0) mean_ssim = 1.0;
    
    return mean_ssim;
}

// ============================================================================
// SSIM计算 - YUV格式
// ============================================================================

double BufferComparator::calculateSSIM_YUV_Y(
    const ImageMeta& img1, const ImageMeta& img2,
    const FormatInfo& info1, const FormatInfo& info2
) {
    // ImageMeta 统一了 Mat 和 AVFrame 路径
    uint8_t* data1 = img1.planeData(0);
    uint8_t* data2 = img2.planeData(0);
    
    if (!data1 || !data2) {
        return 0.0;
    }
    
    return calculateSSIM(data1, data2, info1.width, info1.height,
                        img1.linesize(0), img2.linesize(0));
}

double BufferComparator::calculateSSIM_YUV_U(
    const ImageMeta& img1, const ImageMeta& img2,
    const FormatInfo& info1, const FormatInfo& info2
) {
    // Mat 单通道没有 U 平面，视为完全一致
    if (info1.is_mat || info2.is_mat) {
        return 1.0;
    }

    uint8_t* data1 = img1.planeData(1);
    uint8_t* data2 = img2.planeData(1);
    
    if (!data1 || !data2) {
        return 1.0;  // 无U平面，认为一致
    }
    
    int uv_width = info1.width / 2;
    int uv_height = info1.height / 2;
    
    return calculateSSIM(data1, data2, uv_width, uv_height,
                        img1.linesize(1), img2.linesize(1));
}

double BufferComparator::calculateSSIM_YUV_V(
    const ImageMeta& img1, const ImageMeta& img2,
    const FormatInfo& info1, const FormatInfo& info2
) {
    // Mat 单通道没有 V 平面，视为完全一致
    if (info1.is_mat || info2.is_mat) {
        return 1.0;
    }

    uint8_t* data1 = img1.planeData(2);
    uint8_t* data2 = img2.planeData(2);
    
    if (!data1 || !data2) {
        return 1.0;
    }
    
    int uv_width = info1.width / 2;
    int uv_height = info1.height / 2;
    
    return calculateSSIM(data1, data2, uv_width, uv_height,
                        img1.linesize(2), img2.linesize(2));
}

// ============================================================================
// SSIM计算 - RGB格式
// ============================================================================

double BufferComparator::calculateSSIM_RGB_R(
    const ImageMeta& img1, const ImageMeta& img2,
    const FormatInfo& info1, const FormatInfo& info2
) {
    if (!img1.isValid() || !img2.isValid()) return 0.0;
    
    std::vector<uint8_t> channel1(info1.width * info1.height);
    std::vector<uint8_t> channel2(info2.width * info2.height);
    
    if (!extractRGBChannel(img1, info1.format, info1.width, info1.height, 0, channel1.data()) ||
        !extractRGBChannel(img2, info2.format, info2.width, info2.height, 0, channel2.data())) {
        return 0.0;
    }
    
    return calculateSSIM(channel1.data(), channel2.data(), 
                       info1.width, info1.height,
                       info1.width, info2.width);
}

double BufferComparator::calculateSSIM_RGB_G(
    const ImageMeta& img1, const ImageMeta& img2,
    const FormatInfo& info1, const FormatInfo& info2
) {
    if (!img1.isValid() || !img2.isValid()) return 0.0;
    
    std::vector<uint8_t> channel1(info1.width * info1.height);
    std::vector<uint8_t> channel2(info2.width * info2.height);
    
    if (!extractRGBChannel(img1, info1.format, info1.width, info1.height, 1, channel1.data()) ||
        !extractRGBChannel(img2, info2.format, info2.width, info2.height, 1, channel2.data())) {
        return 0.0;
    }
    
    return calculateSSIM(channel1.data(), channel2.data(), 
                       info1.width, info1.height,
                       info1.width, info2.width);
}

double BufferComparator::calculateSSIM_RGB_B(
    const ImageMeta& img1, const ImageMeta& img2,
    const FormatInfo& info1, const FormatInfo& info2
) {
    if (!img1.isValid() || !img2.isValid()) return 0.0;
    
    std::vector<uint8_t> channel1(info1.width * info1.height);
    std::vector<uint8_t> channel2(info2.width * info2.height);
    
    if (!extractRGBChannel(img1, info1.format, info1.width, info1.height, 2, channel1.data()) ||
        !extractRGBChannel(img2, info2.format, info2.width, info2.height, 2, channel2.data())) {
        return 0.0;
    }
    
    return calculateSSIM(channel1.data(), channel2.data(), 
                       info1.width, info1.height,
                       info1.width, info2.width);
}

// ============================================================================
// 缩放帧（处理 pp 模式分辨率变化）
// ============================================================================

AVFrame* BufferComparator::rescaleFrame(AVFrame* src_frame, int dst_width, int dst_height) {
    if (!src_frame) return nullptr;
    if (src_frame->width == dst_width && src_frame->height == dst_height) {
        return av_frame_clone(src_frame);
    }

    AVFrame* dst_frame = av_frame_alloc();
    if (!dst_frame) return nullptr;

    dst_frame->format = src_frame->format;
    dst_frame->width = dst_width;
    dst_frame->height = dst_height;

    if (av_frame_get_buffer(dst_frame, 0) < 0) {
        av_frame_free(&dst_frame);
        return nullptr;
    }

    SwsContext* sws_ctx = sws_getContext(
        src_frame->width, src_frame->height, static_cast<AVPixelFormat>(src_frame->format),
        dst_width, dst_height, static_cast<AVPixelFormat>(src_frame->format),
        SWS_BICUBIC, nullptr, nullptr, nullptr
    );

    if (!sws_ctx) {
        av_frame_free(&dst_frame);
        return nullptr;
    }

    sws_scale(sws_ctx, src_frame->data, src_frame->linesize, 0, src_frame->height,
              dst_frame->data, dst_frame->linesize);

    sws_freeContext(sws_ctx);

    LOG_DEBUG_FMT("[BufferComparator] Rescaled ref frame: %dx%d → %dx%d",
                  src_frame->width, src_frame->height, dst_width, dst_height);
    return dst_frame;
}

} // namespace io
} // namespace consumptionline
