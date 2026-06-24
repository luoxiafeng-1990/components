#include "../../include/decoder/DecoderFactory.hpp"
#include "../../include/decoder/FFmpegDecoder.hpp"
#include <cstring>
#include <strings.h>  // for strcasecmp

std::unique_ptr<IDecoder> DecoderFactory::createDecoder(DecoderType type) {
    switch (type) {
        case DecoderType::AUTO:
            // 自动选择：优先尝试硬件解码，失败则回退到FFmpeg软件解码
            // TODO: 实现硬件解码器后，这里先尝试创建硬件解码器
            // 目前直接返回FFmpeg软件解码器
            return std::make_unique<FFmpegDecoder>();
            
        case DecoderType::FFMPEG:
            return std::make_unique<FFmpegDecoder>();
            
        case DecoderType::HARDWARE:
            // TODO: 实现硬件解码器
            // return std::make_unique<HardwareDecoder>();
            return nullptr;
            
        case DecoderType::VAAPI:
            // TODO: 实现VA-API解码器
            // return std::make_unique<VaapiDecoder>();
            return nullptr;
            
        case DecoderType::NVDEC:
            // TODO: 实现NVDEC解码器
            // return std::make_unique<NvdecDecoder>();
            return nullptr;
            
        case DecoderType::VIDEOTOOLBOX:
            // TODO: 实现VideoToolbox解码器
            // return std::make_unique<VideoToolboxDecoder>();
            return nullptr;
            
        case DecoderType::CUSTOM:
            // 自定义解码器需要通过其他方式注册和创建
            return nullptr;
            
        default:
            return nullptr;
    }
}

std::unique_ptr<IDecoder> DecoderFactory::createDecoderByName(const char* type_name) {
    if (!type_name) {
        return createDecoder(DecoderType::AUTO);
    }
    
    // 不区分大小写的字符串匹配
    if (strcasecmp(type_name, "auto") == 0) {
        return createDecoder(DecoderType::AUTO);
    } else if (strcasecmp(type_name, "ffmpeg") == 0) {
        return createDecoder(DecoderType::FFMPEG);
    } else if (strcasecmp(type_name, "hardware") == 0 || strcasecmp(type_name, "hw") == 0) {
        return createDecoder(DecoderType::HARDWARE);
    } else if (strcasecmp(type_name, "vaapi") == 0) {
        return createDecoder(DecoderType::VAAPI);
    } else if (strcasecmp(type_name, "nvdec") == 0 || strcasecmp(type_name, "nvidia") == 0) {
        return createDecoder(DecoderType::NVDEC);
    } else if (strcasecmp(type_name, "videotoolbox") == 0 || strcasecmp(type_name, "vt") == 0) {
        return createDecoder(DecoderType::VIDEOTOOLBOX);
    } else if (strcasecmp(type_name, "custom") == 0) {
        return createDecoder(DecoderType::CUSTOM);
    }
    
    // 未识别的类型，返回AUTO
    return createDecoder(DecoderType::AUTO);
}

bool DecoderFactory::isDecoderAvailable(DecoderType type) {
    switch (type) {
        case DecoderType::AUTO:
            return true;  // AUTO总是可用（会回退到FFmpeg）
            
        case DecoderType::FFMPEG:
            return true;  // FFmpeg总是可用
            
        case DecoderType::HARDWARE:
        case DecoderType::VAAPI:
        case DecoderType::NVDEC:
        case DecoderType::VIDEOTOOLBOX:
        case DecoderType::CUSTOM:
            // TODO: 实现硬件解码器后，检查实际可用性
            return false;
            
        default:
            return false;
    }
}

const char* DecoderFactory::getDecoderTypeName(DecoderType type) {
    switch (type) {
        case DecoderType::AUTO:
            return "AUTO";
        case DecoderType::FFMPEG:
            return "FFMPEG";
        case DecoderType::HARDWARE:
            return "HARDWARE";
        case DecoderType::VAAPI:
            return "VAAPI";
        case DecoderType::NVDEC:
            return "NVDEC";
        case DecoderType::VIDEOTOOLBOX:
            return "VIDEOTOOLBOX";
        case DecoderType::CUSTOM:
            return "CUSTOM";
        default:
            return "UNKNOWN";
    }
}

DecoderFactory::DecoderType DecoderFactory::getRecommendedDecoderType() {
    // TODO: 根据平台检测推荐的解码器
    // Linux: 检查VA-API、NVDEC
    // macOS: VideoToolbox
    // Windows: NVDEC、DXVA
    
    // 目前返回FFmpeg软件解码
    return DecoderType::FFMPEG;
}



