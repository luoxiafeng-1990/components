#ifndef DECODER_CONFIG_VALIDATE_HPP
#define DECODER_CONFIG_VALIDATE_HPP

#include "productionline/worker/WorkerConfig.hpp"
#include <string>

/// 校验 DecoderConfig 通用字段 + vendor 扩展（若有）
inline bool validateDecoderConfig(const WorkerConfig::DecoderConfig& d, std::string& err) {
    if (d.decode_threads < 0) {
        err = "DecoderConfig: decode_threads < 0";
        return false;
    }
    if (d.vendor && !d.vendor->validate(err)) {
        return false;
    }
    return true;
}

#endif
