#include "bufferpool/pool/base/BufferPoolBuilderFactory.hpp"
#include "bufferpool/pool/base/BufferPoolBuilder.hpp"
#include "vendor/contracts/MemoryProviderRegistry.hpp"
#include "vendor/contracts/MallocMemoryProvider.hpp"
#include "common/Logger.hpp"
#include <stdio.h>
#include <string.h>

std::unique_ptr<IBufferPoolBuilder> BufferPoolBuilderFactory::create(
    AllocatorType type
) {
    if (type == AllocatorType::AUTO) {
        type = AllocatorType::AVFRAME;
    }

    auto logger = log4cplus::Logger::getInstance(
        LOG4CPLUS_TEXT("components.PoolBuilder.Factory"));

    switch (type) {
        case AllocatorType::AVFRAME:
            LOG4CPLUS_DEBUG(logger, "创建 BufferPoolBuilder(AVFRAME)");
            return BufferPoolBuilder::forAVFrame();

        case AllocatorType::MAT:
            LOG4CPLUS_DEBUG(logger, "创建 BufferPoolBuilder(MAT)");
            return BufferPoolBuilder::forMat();

        case AllocatorType::CONTINUOUS_PHYSICAL:
            LOG4CPLUS_DEBUG(logger, "创建 BufferPoolBuilder(RAW, default malloc)");
            return BufferPoolBuilder::forPhysicalMemory(
                std::make_unique<MallocMemoryProvider>(64));

        default:
            LOG4CPLUS_WARN(logger, "Unknown type, using BufferPoolBuilder(AVFRAME)");
            return BufferPoolBuilder::forAVFrame();
    }
}

std::unique_ptr<IBufferPoolBuilder> BufferPoolBuilderFactory::create(
    AllocatorType type,
    std::unique_ptr<IMemoryProvider> provider)
{
    auto logger = log4cplus::Logger::getInstance(
        LOG4CPLUS_TEXT("components.PoolBuilder.Factory"));

    switch (type) {
        case AllocatorType::CONTINUOUS_PHYSICAL:
        case AllocatorType::AUTO:
            LOG4CPLUS_DEBUG_FMT(logger,
                "创建 BufferPoolBuilder(RAW, provider=%s)",
                provider ? provider->kind() : "null");
            return BufferPoolBuilder::forPhysicalMemory(std::move(provider));

        default:
            LOG4CPLUS_WARN_FMT(logger,
                "类型 %s 不支持 IMemoryProvider 注入，回退到默认",
                typeToString(type));
            return create(type);
    }
}

std::unique_ptr<IBufferPoolBuilder> BufferPoolBuilderFactory::createWithProvider(
    AllocatorType type,
    const std::string& provider_kind)
{
    auto logger = log4cplus::Logger::getInstance(
        LOG4CPLUS_TEXT("components.PoolBuilder.Factory"));

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

std::unique_ptr<IBufferPoolBuilder> BufferPoolBuilderFactory::createByName(
    const char* name)
{
    auto logger = log4cplus::Logger::getInstance(
        LOG4CPLUS_TEXT("components.PoolBuilder.Factory"));

    if (!name) {
        LOG4CPLUS_WARN(logger, "Null name, using BufferPoolBuilder(AVFRAME)");
        return create(AllocatorType::AVFRAME);
    }

    if (strcmp(name, "avframe") == 0) {
        return create(AllocatorType::AVFRAME);
    } else if (strcmp(name, "mat") == 0) {
        return create(AllocatorType::MAT);
    } else if (strcmp(name, "continuous_physical") == 0 ||
               strcmp(name, "framebuffer") == 0) {
        return create(AllocatorType::CONTINUOUS_PHYSICAL);
    } else if (strcmp(name, "auto") == 0) {
        return create(AllocatorType::AUTO);
    }

    LOG4CPLUS_WARN_FMT(logger, "Unknown type: %s, using BufferPoolBuilder(AVFRAME)", name);
    return create(AllocatorType::AVFRAME);
}

const char* BufferPoolBuilderFactory::typeToString(AllocatorType type) {
    switch (type) {
        case AllocatorType::AUTO:                 return "AUTO";
        case AllocatorType::AVFRAME:              return "AVFRAME";
        case AllocatorType::MAT:                  return "MAT";
        case AllocatorType::CONTINUOUS_PHYSICAL:   return "CONTINUOUS_PHYSICAL";
        default:                                   return "UNKNOWN";
    }
}
