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
    LOG_WARN("╔═══════════════════════════════════════════════════════════════╗");
    LOG_WARN("║  ⚠️  Codec Mismatch Detected                                ║");
    LOG_WARN("╚═══════════════════════════════════════════════════════════════╝");
    LOG_WARN_FMT("  Configured decoder: '%s' (expects %s)",
                 decoder_name.c_str(),
                 getCodecFriendlyName(expected_codec_id).c_str());
    LOG_WARN_FMT("  Actual stream codec: %s",
                 getCodecFriendlyName(actual_codec_id).c_str());
    LOG_WARN("");
    LOG_WARN("  💡 Suggestions:");
    LOG_WARN_FMT("  - Update config to use '%s' decoder",
                 avcodec_get_name(actual_codec_id));
    LOG_WARN("  - Or remove decoder name from config for auto-detection");
    LOG_WARN("");
    LOG_WARN("  ⚙️  Continuing with auto-selected decoder...");
    LOG_WARN("╚═══════════════════════════════════════════════════════════════╝");
}

