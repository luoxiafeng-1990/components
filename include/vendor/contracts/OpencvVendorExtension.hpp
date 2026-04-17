#ifndef OPENCV_VENDOR_EXTENSION_HPP
#define OPENCV_VENDOR_EXTENSION_HPP

#include <memory>
#include <string>

/**
 * @brief OpenCV 消费厂商扩展接口
 *
 * 各厂商独立实现本接口以提供特定的图像处理加速配置
 * （如硬件加速后端选择、私有色彩空间转换参数等）。
 * ConsumerTypeConfig::OpencvType 通过 unique_ptr 持有。
 */
class IOpencvVendorExtension {
public:
    virtual ~IOpencvVendorExtension() = default;

    virtual const char* kind() const noexcept = 0;

    virtual std::unique_ptr<IOpencvVendorExtension> clone() const = 0;

    virtual bool validate(std::string& err) const;
};

inline bool IOpencvVendorExtension::validate(std::string& /*err*/) const {
    return true;
}

#endif // OPENCV_VENDOR_EXTENSION_HPP
