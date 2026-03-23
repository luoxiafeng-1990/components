#ifndef DISPLAY_VENDOR_EXTENSION_HPP
#define DISPLAY_VENDOR_EXTENSION_HPP

#include <memory>
#include <string>

/**
 * @brief 显示链路厂商扩展接口
 *
 * 各厂商（tacopro / taco / …）独立实现本接口。
 * WorkerConfig 通过 unique_ptr<IDisplayVendorExtension> 持有厂商参数，
 * 拷贝 WorkerConfig 时调 clone() 深拷贝。
 */
class IDisplayVendorExtension {
public:
    virtual ~IDisplayVendorExtension() = default;

    /// 稳定标识，如 "tacopro"、"taco"
    virtual const char* kind() const noexcept = 0;

    /// 多态深拷贝
    virtual std::unique_ptr<IDisplayVendorExtension> clone() const = 0;

    /// 启动前校验；失败时写入 err
    virtual bool validate(std::string& err) const;
};

inline bool IDisplayVendorExtension::validate(std::string& /*err*/) const {
    return true;
}

#endif // DISPLAY_VENDOR_EXTENSION_HPP
