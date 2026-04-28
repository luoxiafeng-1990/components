#ifndef TACO_MEMORY_PROVIDER_HPP
#define TACO_MEMORY_PROVIDER_HPP

#include "vendor/contracts/IMemoryProvider.hpp"
#include <log4cplus/logger.h>
#include <string>

/**
 * @brief TACO 物理连续内存提供者
 *
 * 通过 taco_sys_get_block / taco_sys_handle2_phys_addr / taco_sys_mmap_noncache
 * 分配物理连续内存，适用于 DMA / Framebuffer 场景。
 */
class TacoMemoryProvider : public IMemoryProvider {
public:
    explicit TacoMemoryProvider(const std::string& zone_name = "default");

    MemoryBlock allocate(size_t size, size_t alignment = 0) override;
    void deallocate(MemoryBlock& block) override;

    const char* kind() const noexcept override { return "taco"; }

    MemoryProviderCapabilities getCapabilities() const override;

    std::unique_ptr<IMemoryProvider> clone() const override;

private:
    std::string zone_name_;
    log4cplus::Logger logger_;
};

/**
 * @brief 注册 TACO 内存提供者到 MemoryProviderRegistry。
 * 
 * display 代码应在 createBufferPool 中调用此函数，确保 provider 在
 * createWithProvider("taco") 之前完成注册。
 * 
 * 不能使用静态初始化（匿名 namespace / constructor attribute），
 * 因为链接器会丢弃未被外部引用的目标文件。
 */
void register_taco_memory_provider();

#endif // TACO_MEMORY_PROVIDER_HPP
