#ifndef TACO_DECODER_EXTENSION_HPP
#define TACO_DECODER_EXTENSION_HPP

#include "vendor/contracts/DecoderVendorExtension.hpp"
#include "vendor/taco/decode/TacoDecoderConfig.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include <cstring>
#include <memory>

class TacoDecoderExtension : public IDecoderVendorExtension {
public:
    TacoConfig config;

    const char* kind() const noexcept override { return "taco"; }

    std::unique_ptr<IDecoderVendorExtension> clone() const override;

    bool validate(std::string& err) const override;
};

/// 从注册表或 useTaco 创建的 vendor 中取得 TACO 扩展；kind 不符返回 nullptr
TacoDecoderExtension* tacoVendorExtension(WorkerConfig::DecoderConfig& d);
const TacoDecoderExtension* tacoVendorExtension(const WorkerConfig::DecoderConfig& d);

TacoConfig* tacoDecoderConfig(WorkerConfig::DecoderConfig& d);
const TacoConfig* tacoDecoderConfig(const WorkerConfig::DecoderConfig& d);

std::unique_ptr<TacoDecoderExtension> makeTacoDecoderExtension(const TacoConfig& cfg);

/// 动态库导出符号（与 TacoDecoderPluginLoader / dlsym 配合）
extern "C" void register_taco_decoder_vendor();

#endif
