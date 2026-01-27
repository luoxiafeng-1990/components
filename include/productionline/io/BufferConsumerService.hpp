/**
 * @file BufferConsumerService.hpp
 * @brief 通用 Buffer 消费服务
 * 
 * 提供统一的 BufferPool 消费机制，支持：
 * - 超时控制
 * - 进度回调
 * - 优雅终止
 * - 统计信息收集
 * 
 * 设计理念：
 * - 通用性：不仅限于测试使用，任何需要消费 BufferPool 的场景都可以使用
 * - 独立性：不依赖具体的业务逻辑，通过回调函数处理每帧数据
 * - 可控性：支持外部中断、超时检测、帧数限制
 * 
 * @version 3.1
 */

#ifndef PRODUCTIONLINE_IO_BUFFER_CONSUMER_SERVICE_HPP
#define PRODUCTIONLINE_IO_BUFFER_CONSUMER_SERVICE_HPP

#include <functional>
#include <memory>
#include <atomic>
#include <chrono>
#include <string>
#include <log4cplus/logger.h>

// 包含完整类型定义
#include "buffer/bufferpool/BufferPool.hpp"
#include "buffer/bufferpool/Buffer.hpp"

namespace productionline {
namespace io {

/**
 * @brief Buffer 消费服务
 * 
 * 封装 BufferPool 的消费逻辑，提供统一的消费接口。
 * 
 * 使用示例：
 * @code
 * BufferConsumerService consumer;
 * 
 * // 设置选项
 * BufferConsumerService::Options opts;
 * opts.max_frames = 300;
 * opts.timeout_ms = 100;
 * 
 * // 开始消费
 * auto result = consumer.consume(pool, [](Buffer* buf, int idx) {
 *     // 处理每一帧
 *     return true; // 返回 false 停止消费
 * }, opts);
 * 
 * printf("Consumed %d frames, %.2f fps\n", 
 *        result.frames_consumed, result.average_fps);
 * @endcode
 */
class BufferConsumerService {
public:
    /**
     * @brief 消费选项
     */
    struct Options {
        int timeout_ms;                 ///< 单次获取 Buffer 超时（毫秒）
        int max_timeout_count;          ///< 最大连续超时次数（达到后视为流结束）
        int max_frames;                 ///< 最大处理帧数（-1=无限制）
        int report_interval;            ///< 进度报告间隔（每 N 帧报告一次）
        bool verbose;                   ///< 是否输出详细日志
        
        Options() 
            : timeout_ms(100)
            , max_timeout_count(10)
            , max_frames(-1)
            , report_interval(100)
            , verbose(false) 
        {}
    };
    
    /**
     * @brief 消费结果
     */
    struct Result {
        int frames_consumed = 0;        ///< 消费的帧数
        int timeout_count = 0;          ///< 超时次数
        double elapsed_seconds = 0.0;   ///< 消耗时间（秒）
        double average_fps = 0.0;       ///< 平均帧率
        bool completed = false;         ///< 是否正常完成（非中断）
        std::string error_message;      ///< 错误信息（如果有）
        
        Result() = default;
    };
    
    /**
     * @brief Buffer 处理回调
     * 
     * @param buffer 当前 Buffer 指针
     * @param frame_index 帧索引（从 0 开始）
     * @return true 继续消费，false 停止消费
     */
    using BufferCallback = std::function<bool(Buffer* buffer, int frame_index)>;
    
    /**
     * @brief 进度回调
     * 
     * @param frames_consumed 已消费帧数
     * @param elapsed_seconds 已消耗时间
     */
    using ProgressCallback = std::function<void(int frames_consumed, double elapsed_seconds)>;
    
public:
    BufferConsumerService();
    ~BufferConsumerService();
    
    /**
     * @brief 消费 BufferPool 中的数据
     * 
     * @param pool BufferPool 共享指针
     * @param callback 每帧处理回调
     * @param options 消费选项
     * @return 消费结果
     */
    Result consume(
        std::shared_ptr<BufferPool> pool,
        BufferCallback callback,
        const Options& options = Options()
    );
    
    /**
     * @brief 带进度报告的消费
     * 
     * @param pool BufferPool 共享指针
     * @param callback 每帧处理回调
     * @param progress_callback 进度回调
     * @param options 消费选项
     * @return 消费结果
     */
    Result consumeWithProgress(
        std::shared_ptr<BufferPool> pool,
        BufferCallback callback,
        ProgressCallback progress_callback,
        const Options& options = Options()
    );
    
    /**
     * @brief 请求停止消费
     * 
     * 线程安全，可以从其他线程调用
     */
    void requestStop();
    
    /**
     * @brief 检查是否正在运行
     */
    bool isRunning() const;
    
    /**
     * @brief 重置状态（用于重新开始）
     */
    void reset();

private:
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    
    // 模块级日志
    log4cplus::Logger logger_;
};

} // namespace io
} // namespace productionline

#endif // PRODUCTIONLINE_IO_BUFFER_CONSUMER_SERVICE_HPP
