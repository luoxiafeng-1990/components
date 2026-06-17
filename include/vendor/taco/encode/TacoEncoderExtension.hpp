#ifndef TACO_ENCODER_EXTENSION_HPP
#define TACO_ENCODER_EXTENSION_HPP

#include "vendor/contracts/EncoderVendorExtension.hpp"
#include <memory>

/**
 * @brief TACO 硬件编码器厂商专用参数
 *
 * 通过 IEncoderVendorExtension 机制注入 WorkerConfig::EncoderConfig::vendor。
 */
class TacoEncoderExtension : public IEncoderVendorExtension {
public:
    int profile = 0;
    int level   = 0;

    const char* kind() const noexcept override { return "taco"; }

    std::unique_ptr<IEncoderVendorExtension> clone() const override {
        return std::make_unique<TacoEncoderExtension>(*this);
    }

    bool applyToCodecContext(void* priv_data) const override;
};

inline std::unique_ptr<TacoEncoderExtension> makeTacoEncoderExtension(int profile = 0, int level = 0) {
    auto ext = std::make_unique<TacoEncoderExtension>();
    ext->profile = profile;
    ext->level   = level;
    return ext;
}

#endif
