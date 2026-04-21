#include "buffer/BufferAllocatorFactory.hpp"
#include "buffer/NormalAllocator.hpp"
#include "buffer/AVFrameAllocator.hpp"
#include "buffer/FramebufferAllocator.hpp"
#include "buffer/MatAllocator.hpp"
#include "vendor/contracts/MemoryProviderRegistry.hpp"
#include "common/Logger.hpp"
#include <stdio.h>
#include <string.h>

// ============================================================================
// 公共接口
// ============================================================================

/**
 * @brief 简化版create - 推荐使用
 * 
 * 工厂内部决定每种类型的最优配置
 */
std::unique_ptr<BufferAllocatorBase> BufferAllocatorFactory::create(
    AllocatorType type
) {
    // AUTO 类型默认使用 NormalAllocator
    if (type == AllocatorType::AUTO) {
        type = AllocatorType::NORMAL;
    }
    
    // 根据类型选择最优配置（工厂策略）
    switch (type) {
        case AllocatorType::NORMAL:
            LOG4CPLUS_DEBUG_FMT(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Allocator.Factory")), "创建NormalAllocator (MALLOC, 64-byte aligned)");
            return std::make_unique<NormalAllocator>(
                BufferMemoryAllocatorType::NORMAL_MALLOC,
                64
            );
            
        case AllocatorType::AVFRAME:
            LOG4CPLUS_DEBUG(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Allocator.Factory")), "创建AVFrameAllocator");
            return std::make_unique<AVFrameAllocator>();
            
        case AllocatorType::FRAMEBUFFER:
            LOG4CPLUS_DEBUG(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Allocator.Factory")), "创建FramebufferAllocator");
            return std::make_unique<FramebufferAllocator>();
        
        case AllocatorType::MAT:
            LOG4CPLUS_DEBUG(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Allocator.Factory")), "创建MatAllocator");
            return std::make_unique<MatAllocator>();
            
        default:
            LOG4CPLUS_WARN_FMT(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Allocator.Factory")), "Unknown type, using NormalAllocator");
            return std::make_unique<NormalAllocator>(
                BufferMemoryAllocatorType::NORMAL_MALLOC,
                64
            );
    }
}

/**
 * @brief 完整版create - 用于特殊配置需求
 */
std::unique_ptr<BufferAllocatorBase> BufferAllocatorFactory::createWithConfig(
    AllocatorType type,
    BufferMemoryAllocatorType mem_type,
    size_t alignment
) {
    // AUTO 类型默认使用 NormalAllocator
    if (type == AllocatorType::AUTO) {
        type = AllocatorType::NORMAL;
    }
    
    return createByType(type, mem_type, alignment);
}

std::unique_ptr<BufferAllocatorBase> BufferAllocatorFactory::createByName(
    const char* name,
    BufferMemoryAllocatorType mem_type,
    size_t alignment
) {
    if (!name) {
        LOG4CPLUS_WARN_FMT(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Allocator.Factory")), "Null name, using NormalAllocator");
        return createByType(AllocatorType::NORMAL, mem_type, alignment);
    }
    
    if (strcmp(name, "normal") == 0) {
        return createByType(AllocatorType::NORMAL, mem_type, alignment);
    } else if (strcmp(name, "avframe") == 0) {
        return createByType(AllocatorType::AVFRAME, mem_type, alignment);
    } else if (strcmp(name, "framebuffer") == 0) {
        return createByType(AllocatorType::FRAMEBUFFER, mem_type, alignment);
    } else if (strcmp(name, "mat") == 0) {
        return createWithConfig(AllocatorType::MAT, mem_type, alignment);
    } else if (strcmp(name, "auto") == 0) {
        return createWithConfig(AllocatorType::AUTO, mem_type, alignment);
    }
    
    LOG4CPLUS_WARN_FMT(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Allocator.Factory")), "Unknown type: %s, using NormalAllocator", name);
    return createByType(AllocatorType::NORMAL, mem_type, alignment);
}

const char* BufferAllocatorFactory::typeToString(AllocatorType type) {
    switch (type) {
        case AllocatorType::AUTO:        return "AUTO";
        case AllocatorType::NORMAL:     return "NORMAL";
        case AllocatorType::AVFRAME:    return "AVFRAME";
        case AllocatorType::FRAMEBUFFER: return "FRAMEBUFFER";
        case AllocatorType::MAT:        return "MAT";
        default:                         return "UNKNOWN";
    }
}

// ============================================================================
// v3.0 新增工厂方法
// ============================================================================

std::unique_ptr<BufferAllocatorBase> BufferAllocatorFactory::create(
    AllocatorType type,
    std::unique_ptr<IMemoryProvider> provider)
{
    auto logger = log4cplus::Logger::getInstance(
        LOG4CPLUS_TEXT("components.Allocator.Factory"));

    switch (type) {
        case AllocatorType::NORMAL:
        case AllocatorType::AUTO:
            LOG4CPLUS_DEBUG_FMT(logger,
                "创建 NormalAllocator (provider=%s)",
                provider ? provider->kind() : "null");
            return std::make_unique<NormalAllocator>(std::move(provider));

        case AllocatorType::FRAMEBUFFER:
            LOG4CPLUS_DEBUG_FMT(logger,
                "创建 FramebufferAllocator (provider=%s)",
                provider ? provider->kind() : "null");
            return std::make_unique<FramebufferAllocator>(std::move(provider));

        default:
            LOG4CPLUS_WARN_FMT(logger,
                "类型 %s 不支持 IMemoryProvider 注入，回退到默认",
                typeToString(type));
            return create(type);
    }
}

std::unique_ptr<BufferAllocatorBase> BufferAllocatorFactory::createWithProvider(
    AllocatorType type,
    const std::string& provider_kind)
{
    auto logger = log4cplus::Logger::getInstance(
        LOG4CPLUS_TEXT("components.Allocator.Factory"));

    auto& registry = MemoryProviderRegistry::instance();
    if (!registry.hasProvider(provider_kind)) {
        LOG4CPLUS_ERROR_FMT(logger,
            "MemoryProviderRegistry 中未找到 '%s'，回退到默认",
            provider_kind.c_str());
        return create(type);
    }

    auto provider = registry.create(provider_kind);
    LOG4CPLUS_DEBUG_FMT(logger,
        "从 Registry 创建 provider '%s' 成功", provider_kind.c_str());
    return create(type, std::move(provider));
}

// ============================================================================
// 私有辅助方法
// ============================================================================

std::unique_ptr<BufferAllocatorBase> BufferAllocatorFactory::createByType(
    AllocatorType type,
    BufferMemoryAllocatorType mem_type,
    size_t alignment
) {
    switch (type) {
        case AllocatorType::NORMAL:
            LOG4CPLUS_DEBUG(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Allocator.Factory")), "🏭 [BufferAllocatorFactory] Creating NormalAllocator");
            return std::make_unique<NormalAllocator>(mem_type, alignment);
            
        case AllocatorType::AVFRAME:
            LOG4CPLUS_DEBUG(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Allocator.Factory")), "🏭 [BufferAllocatorFactory] Creating AVFrameAllocator");
            return std::make_unique<AVFrameAllocator>();
            
        case AllocatorType::FRAMEBUFFER:
            LOG4CPLUS_DEBUG(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Allocator.Factory")), "🏭 [BufferAllocatorFactory] Creating FramebufferAllocator");
            return std::make_unique<FramebufferAllocator>();
        
        case AllocatorType::MAT:
            LOG4CPLUS_DEBUG(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Allocator.Factory")), "🏭 [BufferAllocatorFactory] Creating MatAllocator");
            return std::make_unique<MatAllocator>();
            
        default:
            LOG4CPLUS_WARN(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Allocator.Factory")), "⚠️  [BufferAllocatorFactory] Warning: Unknown AllocatorType, using NormalAllocator");
            return std::make_unique<NormalAllocator>(mem_type, alignment);
    }
}

