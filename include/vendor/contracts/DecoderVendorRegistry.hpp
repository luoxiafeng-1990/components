#ifndef DECODER_VENDOR_REGISTRY_HPP
#define DECODER_VENDOR_REGISTRY_HPP

#include "vendor/contracts/DecoderVendorExtension.hpp"
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

/**
 * @brief 按 kind 字符串创建厂商扩展实例（线程安全注册表）
 */
class DecoderVendorRegistry {
public:
    using Vendor = std::function<std::unique_ptr<IDecoderVendorExtension>()>;

    static DecoderVendorRegistry& instance();

    void registerVendor(std::string kind, Vendor f);

    std::unique_ptr<IDecoderVendorExtension> create(const std::string& kind) const;

private:
    DecoderVendorRegistry() = default;

    mutable std::mutex mu_;
    std::unordered_map<std::string, Vendor> factories_;
};

#endif
