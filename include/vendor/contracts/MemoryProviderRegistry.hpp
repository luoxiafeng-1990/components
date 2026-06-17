#ifndef MEMORY_PROVIDER_REGISTRY_HPP
#define MEMORY_PROVIDER_REGISTRY_HPP

#include "vendor/contracts/IMemoryProvider.hpp"
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

/**
 * @brief 内存提供者注册表（线程安全单例）
 *
 * 与 DecoderVendorRegistry 同构。
 * 厂商实现通过静态初始化或显式调用 registerProvider() 注入工厂函数。
 */
class MemoryProviderRegistry {
public:
    using Factory = std::function<std::unique_ptr<IMemoryProvider>()>;

    static MemoryProviderRegistry& instance();

    void registerProvider(std::string kind, Factory f);

    std::unique_ptr<IMemoryProvider> create(const std::string& kind) const;

    bool hasProvider(const std::string& kind) const;

private:
    MemoryProviderRegistry() = default;

    mutable std::mutex mu_;
    std::unordered_map<std::string, Factory> factories_;
};

#endif // MEMORY_PROVIDER_REGISTRY_HPP
