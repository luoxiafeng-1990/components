#include "bufferpool/pool/base/IBufferPoolBuilder.hpp"
#include "productionline/worker/base/ComponentTopology.hpp"
#include <stdio.h>

std::atomic<uint64_t> IBufferPoolBuilder::next_allocator_id_(1);

IBufferPoolBuilder::~IBufferPoolBuilder() {
}

std::shared_ptr<BufferPool> IBufferPoolBuilder::getPoolSpecialForAllocator(uint64_t pool_id) {
    return ComponentTopology::getInstance().getPoolSpecialForAllocator(pool_id);
}

void IBufferPoolBuilder::unregisterPool(uint64_t pool_id) {
    ComponentTopology::getInstance().unregisterPool(pool_id);
}

std::vector<uint64_t> IBufferPoolBuilder::getPoolsByAllocator() const {
    return ComponentTopology::getInstance().getPoolsByAllocator(getAllocatorId());
}
