#ifndef NPU_INFERENCE_VENDOR_EXTENSION_HPP
#define NPU_INFERENCE_VENDOR_EXTENSION_HPP

#include <memory>
#include <string>

/**
 * @brief NPU 推理消费厂商扩展接口
 *
 * 各厂商独立实现本接口以提供 NPU 专有参数（如硬件核心映射、
 * 私有量化格式、DMA 配置等）。
 * ConsumerTypeConfig::NpuInferenceType 通过 unique_ptr 持有。
 */
class INpuInferenceVendorExtension {
public:
    virtual ~INpuInferenceVendorExtension() = default;

    virtual const char* kind() const noexcept = 0;

    virtual std::unique_ptr<INpuInferenceVendorExtension> clone() const = 0;

    virtual bool validate(std::string& err) const;
};

inline bool INpuInferenceVendorExtension::validate(std::string& /*err*/) const {
    return true;
}

#endif // NPU_INFERENCE_VENDOR_EXTENSION_HPP
