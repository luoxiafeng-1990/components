#include "buffer/BufferAllocatorFactory.hpp"
#include "buffer/NormalAllocator.hpp"
#include "buffer/AVFrameAllocator.hpp"
#include "buffer/FramebufferAllocator.hpp"
#include "buffer/MatAllocator.hpp"
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

