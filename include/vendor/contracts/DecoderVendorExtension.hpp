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

    /**
     * @brief 获取指定通道的 PP 输出宽度
     * @return > 0 时为有效值；0 表示未配置，调用方应使用源分辨率
     */
    virtual int getOutputWidth(int channel = 0) const;

    /**
     * @brief 获取指定通道的 PP 输出高度
     * @return > 0 时为有效值；0 表示未配置，调用方应使用源分辨率
     */
    virtual int getOutputHeight(int channel = 0) const;

    /**
     * @brief 将厂商特有的解码器后处理参数应用到已打开的 AVCodecContext
     *
     * 由 Worker 在获取到源分辨率后、调用 avcodec_open2 前调用。
     * 厂商实现负责：参数校验、硬件能力限制检查、av_opt_set 设置。
     * 如果参数不符合硬件规定，应记录错误信息并返回 false。
     *
     * @param priv_data      AVCodecContext::priv_data（void* 避免在接口层引入 FFmpeg 头）
     * @param source_width   输入视频的实际宽度
     * @param source_height  输入视频的实际高度
     * @return true 成功，false 参数不合法（Worker 应终止初始化）
     */
    virtual bool applyToCodecContext(void* priv_data,
                                     int source_width, int source_height);

    /**
     * @brief 根据编解码器参数自动配置厂商扩展（如 B 帧探测 → reorder 策略）
     *
     * 由 Worker 在 applyToCodecContext() 之前调用。
     * 厂商实现可根据 codec_id、profile、video_delay 等信息动态调整配置。
     * 默认实现为空操作。
     *
     * @param codec_id      编解码器 ID（AVCodecID）
     * @param profile       编码 Profile（如 H.264 Baseline=66, Main=77, High=100）
     * @param video_delay   B 帧导致的参考帧延迟数（> 0 表示有 B 帧）
     */
    virtual void autoConfigureFromCodecParams(int codec_id, int profile, int video_delay);
};

inline bool IDecoderVendorExtension::validate(std::string& /*err*/) const {
    return true;
}

inline double IDecoderVendorExtension::getChannelBytesPerPixel(
    int /*channel*/, void* /*priv_data*/, int /*pix_fmt*/) const {
    return -1.0;
}

inline int IDecoderVendorExtension::getOutputWidth(int /*channel*/) const {
    return 0;
}

inline int IDecoderVendorExtension::getOutputHeight(int /*channel*/) const {
    return 0;
}

inline bool IDecoderVendorExtension::applyToCodecContext(
    void* /*priv_data*/, int /*source_width*/, int /*source_height*/) {
    return true;
}

inline void IDecoderVendorExtension::autoConfigureFromCodecParams(
    int /*codec_id*/, int /*profile*/, int /*video_delay*/) {
    // 默认空实现：不做任何自动配置
}

#endif
