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

    /**
     * @brief 获取指定通道的每像素字节数（厂商多通道硬件解码器专用）
     * @param channel     通道编号（0 = 主通道，1 = 副通道，...）
     * @param priv_data   AVCodecContext::priv_data（void* 避免在接口层引入 FFmpeg 头）
     * @param pix_fmt     AVCodecContext::pix_fmt（int 形式）
     * @return > 0.0 时为有效值；<= 0.0 表示该通道未启用或不支持，调用方应走通用路径
     */
    virtual double getChannelBytesPerPixel(int channel, void* priv_data, int pix_fmt) const;
};

inline bool IDecoderVendorExtension::validate(std::string& /*err*/) const {
    return true;
}

inline double IDecoderVendorExtension::getChannelBytesPerPixel(
    int /*channel*/, void* /*priv_data*/, int /*pix_fmt*/) const {
    return -1.0;
}

#endif
