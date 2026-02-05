#ifndef MAT_ALLOCATOR_HPP
#define MAT_ALLOCATOR_HPP

#include "buffer/BufferAllocatorBase.hpp"
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

#include <opencv2/opencv.hpp>
#include "opencv2/core/tacv.hpp"

class MatAllocator : public BufferAllocatorBase {
public:
    MatAllocator();
    
    ~MatAllocator() override;
    
    Buffer* injectMatToPool(cv::Mat* frame, BufferPool* pool);
    
    bool releaseMat(Buffer* buffer, BufferPool* pool);
    
    uint64_t allocatePoolWithBuffers(
        int count,
        size_t size,
        const std::string& name,
        const std::string& category = ""
    ) override;
    
    Buffer* injectBufferToPool(
        uint64_t pool_id,
        size_t size,
        QueueType queue = QueueType::FREE
    ) override;
    
    Buffer* injectExternalBufferToPool(
        uint64_t pool_id,
        void* virt_addr,
        uint64_t phys_addr,
        size_t size,
        QueueType queue = QueueType::FREE
    ) override;
    
    bool removeBufferFromPool(uint64_t pool_id, Buffer* buffer) override;
    
    bool destroyPool() override;
    
protected:
    Buffer* createBuffer(uint32_t id, size_t size) override;
    
    void deallocateBuffer(Buffer* buffer) override;
    
private:
    std::atomic<uint32_t> next_buffer_id_;
    
    std::mutex mapping_mutex_;
    
    log4cplus::Logger logger_;
};

#endif