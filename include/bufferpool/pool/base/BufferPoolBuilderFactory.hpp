#pragma once

#include "bufferpool/pool/base/IBufferPoolBuilder.hpp"
#include "vendor/contracts/IMemoryProvider.hpp"
#include <memory>
#include <string>

/**
 * @brief BufferPoolBuilderFactory - IBufferPoolBuilder 工厂
 *
 * 根据类型创建合适的 IBufferPoolBuilder 实现。
 */
class BufferPoolBuilderFactory {
public:
    enum class AllocatorType {
        AUTO,
        AVFRAME,
        MAT,
        CONTINUOUS_PHYSICAL
    };

    static std::unique_ptr<IBufferPoolBuilder> create(
        AllocatorType type = AllocatorType::AUTO
    );

    static std::unique_ptr<IBufferPoolBuilder> create(
        AllocatorType type,
        std::unique_ptr<IMemoryProvider> provider
    );

    static std::unique_ptr<IBufferPoolBuilder> createWithProvider(
        AllocatorType type,
        const std::string& provider_kind
    );

    static std::unique_ptr<IBufferPoolBuilder> createByName(
        const char* name
    );

    static const char* typeToString(AllocatorType type);
};
