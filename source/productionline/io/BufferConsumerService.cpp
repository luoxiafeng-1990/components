/**
 * @file BufferConsumerService.cpp
 * @brief BufferConsumerService 实现
 */

#include "productionline/io/BufferConsumerService.hpp"

#include <climits>  // for INT32_MAX
#include <iomanip>  // for std::fixed, std::setprecision
#include <log4cplus/loggingmacros.h>

namespace productionline {
namespace io {

BufferConsumerService::BufferConsumerService()
    : logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.io.BufferConsumer")))
{
}

BufferConsumerService::~BufferConsumerService() {
    requestStop();
}

BufferConsumerService::Result BufferConsumerService::consume(
    std::shared_ptr<BufferPool> pool,
    BufferCallback callback,
    const Options& options
) {
    // 不带进度回调的版本
    return consumeWithProgress(pool, callback, nullptr, options);
}

BufferConsumerService::Result BufferConsumerService::consumeWithProgress(
    std::shared_ptr<BufferPool> pool,
    BufferCallback callback,
    ProgressCallback progress_callback,
    const Options& options
) {
    Result result;
    
    // 验证参数
    if (!pool) {
        result.error_message = "BufferPool is null";
        return result;
    }
    
    if (!callback) {
        result.error_message = "BufferCallback is null";
        return result;
    }
    
    // 初始化状态
    running_ = true;
    stop_requested_ = false;
    
    auto start_time = std::chrono::steady_clock::now();
    int timeout_count = 0;
    int max_frames = options.max_frames > 0 ? options.max_frames : INT32_MAX;
    
    if (options.verbose) {
        LOG4CPLUS_INFO(logger_, "Starting consumption, max_frames=" << options.max_frames 
                       << ", timeout_ms=" << options.timeout_ms);
    }
    
    // 消费循环
    while (running_ && !stop_requested_ && result.frames_consumed < max_frames) {
        // 获取 Buffer
        Buffer* buffer = pool->acquireFilled(true, options.timeout_ms);
        
        if (!buffer) {
            timeout_count++;
            result.timeout_count++;
            
            if (timeout_count >= options.max_timeout_count) {
                // 连续超时达到阈值，认为流结束
                if (options.verbose) {
                    LOG4CPLUS_INFO(logger_, "Max timeout reached (" << options.max_timeout_count 
                                   << "), stopping");
                }
                result.completed = true;
                break;
            }
            continue;
        }
        
        // 重置超时计数
        timeout_count = 0;
        
        // 调用用户回调处理 Buffer
        bool should_continue = false;
        try {
            should_continue = callback(buffer, result.frames_consumed);
        } catch (const std::exception& e) {
            result.error_message = std::string("Callback exception: ") + e.what();
            pool->releaseFilled(buffer);
            break;
        }
        
        // 释放 Buffer
        pool->releaseFilled(buffer);
        
        // 更新计数
        result.frames_consumed++;
        
        // 检查是否继续
        if (!should_continue) {
            if (options.verbose) {
                LOG4CPLUS_INFO(logger_, "Callback returned false, stopping");
            }
            result.completed = true;
            break;
        }
        
        // 进度报告
        if (progress_callback && 
            options.report_interval > 0 && 
            result.frames_consumed % options.report_interval == 0) {
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - start_time).count();
            progress_callback(result.frames_consumed, elapsed);
        }
    }
    
    // 计算最终结果
    auto end_time = std::chrono::steady_clock::now();
    result.elapsed_seconds = std::chrono::duration<double>(end_time - start_time).count();
    
    if (result.elapsed_seconds > 0) {
        result.average_fps = result.frames_consumed / result.elapsed_seconds;
    }
    
    // 检查是否正常完成
    if (result.frames_consumed >= max_frames) {
        result.completed = true;
    }
    
    running_ = false;
    
    if (options.verbose) {
        LOG4CPLUS_INFO(logger_, "Finished, frames=" << result.frames_consumed 
                       << ", fps=" << std::fixed << std::setprecision(2) << result.average_fps 
                       << ", elapsed=" << result.elapsed_seconds << "s");
    }
    
    return result;
}

void BufferConsumerService::requestStop() {
    stop_requested_ = true;
}

bool BufferConsumerService::isRunning() const {
    return running_;
}

void BufferConsumerService::reset() {
    running_ = false;
    stop_requested_ = false;
}

} // namespace io
} // namespace productionline
