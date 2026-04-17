#ifndef ENCODER_VENDOR_EXTENSION_HPP
#define ENCODER_VENDOR_EXTENSION_HPP

#include <memory>
#include <string>

/**
 * @brief 编码器厂商扩展配置接口
 *
 * 具体厂商（如 TACO）在独立头/源中实现本接口。
 * WorkerConfig::EncoderConfig 通过 std::unique_ptr<IEncoderVendorExtension> vendor 持有。
 */
class IEncoderVendorExtension {
public:
    virtual ~IEncoderVendorExtension() = default;

    virtual const char* kind() const noexcept = 0;

    virtual std::unique_ptr<IEncoderVendorExtension> clone() const = 0;

    virtual bool validate(std::string& err) const;

    /**
     * @brief 将厂商特有参数应用到已打开的 AVCodecContext
     * @param priv_data  AVCodecContext::priv_data（void* 避免在接口层引入 FFmpeg 头）
     * @return true 表示成功
     */
    virtual bool applyToCodecContext(void* priv_data) const;
};

inline bool IEncoderVendorExtension::validate(std::string& /*err*/) const {
    return true;
}

inline bool IEncoderVendorExtension::applyToCodecContext(void* /*priv_data*/) const {
    return true;
}

#endif
