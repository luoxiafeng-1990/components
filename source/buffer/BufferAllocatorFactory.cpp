#include "buffer/BufferAllocatorFactory.hpp"
#include "buffer/NormalAllocator.hpp"
#include "buffer/AVFrameAllocator.hpp"
#include "buffer/FramebufferAllocator.hpp"
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
            LOG_DEBUG("[BufferAllocatorFactory] 创建NormalAllocator (MALLOC, 64-byte aligned)");
            return std::make_unique<NormalAllocator>(
                BufferMemoryAllocatorType::NORMAL_MALLOC,
                64
            );
            
        case AllocatorType::AVFRAME:
            LOG_DEBUG("[BufferAllocatorFactory] 创建AVFrameAllocator");
            return std::make_unique<AVFrameAllocator>();
            
        case AllocatorType::FRAMEBUFFER:
            LOG_DEBUG("[BufferAllocatorFactory] 创建FramebufferAllocator");
            return std::make_unique<FramebufferAllocator>();
            
        default:
            LOG_WARN("[BufferAllocatorFactory] Unknown type, using NormalAllocator");
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
        LOG_WARN("[BufferAllocatorFactory] Null name, using NormalAllocator");
        return createByType(AllocatorType::NORMAL, mem_type, alignment);
    }
    
    if (strcmp(name, "normal") == 0) {
        return createByType(AllocatorType::NORMAL, mem_type, alignment);
    } else if (strcmp(name, "avframe") == 0) {
        return createByType(AllocatorType::AVFRAME, mem_type, alignment);
    } else if (strcmp(name, "framebuffer") == 0) {
        return createByType(AllocatorType::FRAMEBUFFER, mem_type, alignment);
    } else if (strcmp(name, "auto") == 0) {
        return createWithConfig(AllocatorType::AUTO, mem_type, alignment);
    }
    
    LOG_WARN_FMT("[BufferAllocatorFactory] Unknown type: %s, using NormalAllocator", name);
    return createByType(AllocatorType::NORMAL, mem_type, alignment);
}

const char* BufferAllocatorFactory::typeToString(AllocatorType type) {
    switch (type) {
        case AllocatorType::AUTO:        return "AUTO";
        case AllocatorType::NORMAL:     return "NORMAL";
        case AllocatorType::AVFRAME:    return "AVFRAME";
        case AllocatorType::FRAMEBUFFER: return "FRAMEBUFFER";
        default:                         return "UNKNOWN";
    }
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
            printf("🏭 BufferAllocatorFactory: Creating NormalAllocator\n");
            return std::make_unique<NormalAllocator>(mem_type, alignment);
            
        case AllocatorType::AVFRAME:
            printf("🏭 BufferAllocatorFactory: Creating AVFrameAllocator\n");
            return std::make_unique<AVFrameAllocator>();
            
        case AllocatorType::FRAMEBUFFER:
            printf("🏭 BufferAllocatorFactory: Creating FramebufferAllocator\n");
            return std::make_unique<FramebufferAllocator>();
            
        default:
            printf("⚠️  Warning: Unknown AllocatorType, using NormalAllocator\n");
            return std::make_unique<NormalAllocator>(mem_type, alignment);
    }
}

