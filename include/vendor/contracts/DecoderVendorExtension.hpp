#ifndef DECODER_VENDOR_EXTENSION_HPP
#define DECODER_VENDOR_EXTENSION_HPP

#include <memory>
#include <string>

/**
 * @brief 解码器厂商扩展配置（方案 B：核心不依赖具体芯片 SDK 类型）
 *
 * 具体厂商（如 TACO）在独立头/源中实现本接口，并通过 DecoderVendorRegistry 注册工厂。
 */
class IDecoderVendorExtension {
public:
    virtual ~IDecoderVendorExtension() = default;

    /// 稳定标识，用于日志 / 工厂查找（如 "taco"）
    virtual const char* kind() const noexcept = 0;

    /// 深拷贝，供 DecoderConfig 拷贝语义使用
    virtual std::unique_ptr<IDecoderVendorExtension> clone() const = 0;

    /// 启动前校验；失败时写入 err
    virtual bool validate(std::string& err) const;
};

inline bool IDecoderVendorExtension::validate(std::string& /*err*/) const {
    return true;
}

#endif
