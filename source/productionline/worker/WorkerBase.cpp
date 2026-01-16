#include "productionline/worker/WorkerBase.hpp"
#include "common/Logger.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
}

// ============================================================================
// 编解码器类型检测工具实现（v2.11 新增）
// ============================================================================

AVCodecID WorkerBase::getExpectedCodecIdFromDecoderName(const std::string& decoder_name) {
    if (decoder_name.empty() || decoder_name == "auto") {
        return AV_CODEC_ID_NONE;  // 不检查
    }
    
    // ========== H.264/AVC 系列 ==========
    if (decoder_name.find("h264") != std::string::npos ||
        decoder_name.find("avc") != std::string::npos) {
        return AV_CODEC_ID_H264;
    }
    
    // ========== H.265/HEVC 系列 ==========
    if (decoder_name.find("h265") != std::string::npos ||
        decoder_name.find("hevc") != std::string::npos) {
        return AV_CODEC_ID_HEVC;
    }
    
    // ========== VP8/VP9 ==========
    if (decoder_name == "vp8" || decoder_name == "libvpx") {
        return AV_CODEC_ID_VP8;
    }
    if (decoder_name == "vp9" || decoder_name == "libvpx-vp9") {
        return AV_CODEC_ID_VP9;
    }
    
    // ========== AV1 ==========
    if (decoder_name.find("av1") != std::string::npos) {
        return AV_CODEC_ID_AV1;
    }
    
    // ========== MPEG-2/MPEG-4 ==========
    if (decoder_name.find("mpeg2") != std::string::npos) {
        return AV_CODEC_ID_MPEG2VIDEO;
    }
    if (decoder_name.find("mpeg4") != std::string::npos) {
        return AV_CODEC_ID_MPEG4;
    }
    
    return AV_CODEC_ID_NONE;  // 未知，不检查
}

std::string WorkerBase::getCodecFriendlyName(AVCodecID codec_id) {
    switch (codec_id) {
        case AV_CODEC_ID_H264:
            return "H.264/AVC";
        case AV_CODEC_ID_HEVC:
            return "H.265/HEVC";
        case AV_CODEC_ID_VP8:
            return "VP8";
        case AV_CODEC_ID_VP9:
            return "VP9";
        case AV_CODEC_ID_AV1:
            return "AV1";
        case AV_CODEC_ID_MPEG2VIDEO:
            return "MPEG-2";
        case AV_CODEC_ID_MPEG4:
            return "MPEG-4";
        default:
            // 对于其他编解码器，使用 FFmpeg 的原始名称
            return avcodec_get_name(codec_id);
    }
}

void WorkerBase::checkCodecMismatch(AVCodecID actual_codec_id, 
                                     const std::string& decoder_name) const {
    AVCodecID expected_codec_id = getExpectedCodecIdFromDecoderName(decoder_name);
    
    // 不需要检查的情况（用户没有指定解码器，或指定的是 "auto"）
    if (expected_codec_id == AV_CODEC_ID_NONE) {
        return;
    }
    
    // 匹配成功，无需警告
    if (actual_codec_id == expected_codec_id) {
        return;
    }
    
    // ⚠️ 不匹配，打印详细警告
    LOG4CPLUS_WARN(logger_, "╔═══════════════════════════════════════════════════════════════╗");
    LOG4CPLUS_WARN(logger_, "║  ⚠️  Codec Mismatch Detected                                ║");
    LOG4CPLUS_WARN(logger_, "╚═══════════════════════════════════════════════════════════════╝");
    LOG4CPLUS_WARN_FMT(logger_, "  Configured decoder: '%s' (expects %s)",
                 decoder_name.c_str(),
                 getCodecFriendlyName(expected_codec_id).c_str());
    LOG4CPLUS_WARN_FMT(logger_, "  Actual stream codec: %s",
                 getCodecFriendlyName(actual_codec_id).c_str());
    LOG4CPLUS_WARN(logger_, "");
    LOG4CPLUS_WARN(logger_, "  💡 Suggestions:");
    LOG4CPLUS_WARN_FMT(logger_, "  - Update config to use '%s' decoder",
                 avcodec_get_name(actual_codec_id));
    LOG4CPLUS_WARN(logger_, "  - Or remove decoder name from config for auto-detection");
    LOG4CPLUS_WARN(logger_, "");
    LOG4CPLUS_WARN(logger_, "  ⚙️  Continuing with auto-selected decoder...");
    LOG4CPLUS_WARN(logger_, "╚═══════════════════════════════════════════════════════════════╝");
}

// ============================================================================
// 编解码器类型检测工具实现（v2.18 新增）
// ============================================================================

bool WorkerBase::isHardwareDecoder(const AVCodec* codec) const {
    if (!codec) {
        return false;
    }
    
    // ⭐ 方法1：检查 AVCodec->capabilities 中的 AV_CODEC_CAP_HARDWARE 标志
    // 这是 FFmpeg 官方推荐的方式，用于标识硬件加速解码器
    if (codec->capabilities & AV_CODEC_CAP_HARDWARE) {
        LOG4CPLUS_DEBUG_FMT(logger_, "[WorkerBase] Codec '%s' is hardware decoder (AV_CODEC_CAP_HARDWARE)", 
                      codec->name);
        return true;
    }
    
    // ⭐ 方法2：检查解码器是否有硬件配置
    // 如果 avcodec_get_hw_config(codec, 0) 返回非空，说明支持硬件加速
    const AVCodecHWConfig* hw_config = avcodec_get_hw_config(codec, 0);
    if (hw_config != nullptr) {
        LOG4CPLUS_DEBUG_FMT(logger_, "[WorkerBase] Codec '%s' is hardware decoder (has hw_config)", 
                      codec->name);
        return true;
    }
    
    // ✅ 两种方法都未检测到硬件特征，判定为软件解码器
    LOG4CPLUS_DEBUG_FMT(logger_, "[WorkerBase] Codec '%s' is software decoder", codec->name);
    return false;
}

const AVCodec* WorkerBase::findPureSoftwareDecoder(AVCodecID codec_id) const {
    LOG4CPLUS_DEBUG_FMT(logger_, "[WorkerBase] Searching for pure software decoder (codec_id=%d)...", codec_id);
    
    const AVCodec* sw_codec = nullptr;
    void* opaque = nullptr;
    
    // ⭐ 遍历所有已注册的解码器
    while ((sw_codec = av_codec_iterate(&opaque)) != nullptr) {
        // 检查条件：
        // 1. 是解码器（不是编码器）
        // 2. 匹配 codec_id
        // 3. 不是硬件解码器（使用 isHardwareDecoder() 判断）
        if (av_codec_is_decoder(sw_codec) && 
            sw_codec->id == codec_id &&
            !isHardwareDecoder(sw_codec)) {
            LOG4CPLUS_INFO_FMT(logger_, "[WorkerBase] ✅ Found pure software decoder: %s", sw_codec->name);
            return sw_codec;
        }
    }
    
    // ❌ 未找到软件解码器
    LOG4CPLUS_ERROR_FMT(logger_, "[WorkerBase] ❌ No pure software decoder found for codec_id=%d", codec_id);
    return nullptr;
}

