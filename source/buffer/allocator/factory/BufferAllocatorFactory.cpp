#include "buffer/allocator/factory/BufferAllocatorFactory.hpp"
#include "buffer/allocator/implementation/NormalAllocator.hpp"
#include "buffer/allocator/implementation/AVFrameAllocator.hpp"
#include "buffer/allocator/implementation/FramebufferAllocator.hpp"
#include <stdio.h>
#include <string.h>

// ============================================================================
// 公共接口
// ============================================================================

std::unique_ptr<BufferAllocatorBase> BufferAllocatorFactory::create(
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
        printf("⚠️  Warning: Null name provided, using NormalAllocator\n");
        return createByType(AllocatorType::NORMAL, mem_type, alignment);
    }
    
    if (strcmp(name, "normal") == 0) {
        return createByType(AllocatorType::NORMAL, mem_type, alignment);
    } else if (strcmp(name, "avframe") == 0) {
        return createByType(AllocatorType::AVFRAME, mem_type, alignment);
    } else if (strcmp(name, "framebuffer") == 0) {
        return createByType(AllocatorType::FRAMEBUFFER, mem_type, alignment);
    } else if (strcmp(name, "auto") == 0) {
        return create(AllocatorType::AUTO, mem_type, alignment);
    }
    
    printf("⚠️  Warning: Unknown allocator type: %s, using NormalAllocator\n", name);
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

