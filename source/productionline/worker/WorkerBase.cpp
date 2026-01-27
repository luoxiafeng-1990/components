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
    
    // ✅ 直接从 FFmpeg 查询：解码器自己知道自己支持什么 codec
    // 这比硬编码字符串匹配更权威、更完整、更易维护
    const AVCodec* codec = avcodec_find_decoder_by_name(decoder_name.c_str());
    if (codec) {
        return codec->id;  // AVCodec 结构体的 id 字段就是它支持的 codec ID
    }
    
    return AV_CODEC_ID_NONE;  // 找不到这个解码器，不检查
}

std::string WorkerBase::getCodecFriendlyName(AVCodecID codec_id) {
    // ✅ 直接从 FFmpeg 获取 codec 的官方长名称
    // 例如: "H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10"
    const AVCodecDescriptor* desc = avcodec_descriptor_get(codec_id);
    if (desc && desc->long_name) {
        return desc->long_name;
    }
    
    // 兜底：如果没有描述符，使用短名称
    return avcodec_get_name(codec_id);
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

