#ifndef IVENDOR_OPTIONS_REGISTRAR_HPP
#define IVENDOR_OPTIONS_REGISTRAR_HPP

#include <memory>

namespace CLI { class App; }
class IDecoderVendorExtension;

/**
 * @brief 解码器厂商 CLI 参数注册接口
 *
 * 每个硬件厂商实现此接口，向 CLI::App 注册自己特有的命令行选项。
 * 核心层（VdecPlugin）只通过此接口交互，不知道具体有哪些厂商参数。
 *
 * 模式与 DataSourceOptions::registerTo() / applyTo() 一致，
 * 与 DisplayPlugin 的 --vendor 分发表模式对齐。
 *
 * 使用方式：
 * @code
 * // 厂商实现（如 TacoVendorOptions）
 * class TacoVendorOptions : public IVendorOptionsRegistrar {
 *     void registerTo(CLI::App& app) override {
 *         app.add_flag("--ch0", ch0_, "启用通道 0");
 *         app.add_flag("--ch1", ch1_, "启用通道 1");
 *     }
 *     ...
 * };
 *
 * // 核心层使用
 * for (auto& [name, registrar] : vendorRegistrars()) {
 *     registrar->registerTo(app);  // 不知道具体参数
 * }
 * @endcode
 */
class IVendorOptionsRegistrar {
public:
    virtual ~IVendorOptionsRegistrar() = default;

    /// 厂商标识（如 "taco"、"nvidia"），与 --vendor 选项值匹配
    virtual const char* name() const noexcept = 0;

    /// 向 CLI::App 注册厂商特有的命令行选项
    virtual void registerTo(CLI::App& app) = 0;

    /// 使用已解析的 CLI 参数构建厂商解码器扩展
    virtual std::unique_ptr<IDecoderVendorExtension> buildExtension() const = 0;
};

#endif // IVENDOR_OPTIONS_REGISTRAR_HPP
