#ifndef BUFFER_MANAGER_HPP
#define BUFFER_MANAGER_HPP

#include "Buffer.hpp"
#include <queue>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <string>
#include <memory>

// 前向声明
class VideoFile;

/**
 * BufferManager - 线程安全的 Buffer 池管理器
 * 
 * 功能：
 * - 管理一组物理连续的 Buffer
 * - 双队列管理（空闲队列 + 就绪队列）
 * - 线程安全的生产者-消费者模式
 * - 支持阻塞等待和非阻塞获取
 * - 内置视频文件生产者线程
 * 
 * 使用场景：
 * - 生产者：从空闲队列获取buffer → 填充数据 → 放入就绪队列
 * - 消费者：从就绪队列获取buffer → 处理数据 → 回收到空闲队列
 */
class BufferManager {
public:
    // ============ 类型定义 ============
    
    /**
     * 错误回调函数类型
     * @param error_msg 错误消息
     */
    using ErrorCallback = std::function<void(const std::string& error_msg)>;
    
    /**
     * 生产者线程状态
     */
    enum class ProducerState {
        STOPPED,    // 已停止
        RUNNING,    // 运行中
        ERROR       // 错误状态
    };
    
public:
    /**
     * 构造函数
     * @param buffer_count Buffer 数量
     * @param buffer_size 每个 Buffer 的大小（字节）
     * @param use_cma 是否使用物理连续内存（CMA）
     */
    BufferManager(int buffer_count, size_t buffer_size, bool use_cma = false);
    
    /**
     * 析构函数：释放所有资源
     */
    ~BufferManager();
    
    // 禁止拷贝
    BufferManager(const BufferManager&) = delete;
    BufferManager& operator=(const BufferManager&) = delete;
    
    // ============ 生产者接口 ============
    
    /**
     * 获取一个空闲的 Buffer（生产者调用）
     * 
     * @param blocking 是否阻塞等待（true: 等待直到有空闲buffer, false: 没有则立即返回nullptr）
     * @param timeout_ms 超时时间（毫秒），仅在 blocking=true 时有效，0表示无限等待
     * @return 空闲的 Buffer 指针，如果没有则返回 nullptr
     */
    Buffer* acquireFreeBuffer(bool blocking = true, int timeout_ms = 0);
    
    /**
     * 提交一个已填充的 Buffer（生产者调用）
     * 将 buffer 从空闲状态转移到就绪状态
     * 
     * @param buffer Buffer 指针
     */
    void submitFilledBuffer(Buffer* buffer);
    
    // ============ 消费者接口 ============
    
    /**
     * 获取一个已填充的 Buffer（消费者调用）
     * 
     * @param blocking 是否阻塞等待（true: 等待直到有就绪buffer, false: 没有则立即返回nullptr）
     * @param timeout_ms 超时时间（毫秒），仅在 blocking=true 时有效，0表示无限等待
     * @return 已填充的 Buffer 指针，如果没有则返回 nullptr
     */
    Buffer* acquireFilledBuffer(bool blocking = true, int timeout_ms = 0);
    
    /**
     * 回收一个已处理的 Buffer（消费者调用）
     * 将 buffer 从就绪状态回收到空闲状态
     * 
     * @param buffer Buffer 指针
     */
    void recycleBuffer(Buffer* buffer);
    
    // ============ 查询接口 ============
    
    /**
     * 获取空闲 Buffer 数量
     */
    int getFreeBufferCount() const;
    
    /**
     * 获取就绪 Buffer 数量
     */
    int getFilledBufferCount() const;
    
    /**
     * 获取总 Buffer 数量
     */
    int getTotalBufferCount() const;
    
    /**
     * 获取每个 Buffer 的大小
     */
    size_t getBufferSize() const;
    
   
    /**
     * 启动多个视频文件生产者线程（多线程模式）
     * 
     * 启动多个生产者线程并行读取视频帧，提高生产效率。
     * 多个线程会协调读取，确保每帧只被读取一次且按顺序填充。
     * 
     * @param thread_count 生产者线程数量（建议2-4个）
     * @param video_file_path 视频文件路径
     * @param width 视频宽度（像素）
     * @param height 视频高度（像素）
     * @param bits_per_pixel 每像素位数（如RGB24=24, RGBA32=32）
     * @param loop 是否循环播放（到达文件末尾后重新开始）
     * @param error_callback 错误回调函数（可选）
     * @return 成功返回true，失败返回false
     */
    bool startMultipleVideoProducers(int thread_count,
                                    const char* video_file_path, 
                                    int width, int height, int bits_per_pixel,
                                    bool loop = false,
                                    ErrorCallback error_callback = nullptr);
    
    /**
     * 启动多个视频文件生产者线程（io_uring高性能模式）
     * 
     * 使用io_uring异步I/O框架，显著提升I/O性能。
     * 
     * 性能优势：
     * - 零拷贝异步I/O
     * - 批量操作，减少系统调用
     * - 降低CPU使用率
     * - 提高I/O吞吐量（2-5倍）
     * 
     * 要求：
     * - Linux内核 >= 5.1
     * - 已安装liburing库
     * 
     * @param thread_count 生产者线程数量（建议2-4个）
     * @param video_file_path 视频文件路径
     * @param width 视频宽度（像素）
     * @param height 视频高度（像素）
     * @param bits_per_pixel 每像素位数（如RGB24=24, RGBA32=32）
     * @param loop 是否循环播放（到达文件末尾后重新开始）
     * @param error_callback 错误回调函数（可选）
     * @return 成功返回true，失败返回false
     */
    bool startMultipleVideoProducersIoUring(int thread_count,
                                           const char* video_file_path, 
                                           int width, int height, int bits_per_pixel,
                                           bool loop = false,
                                           ErrorCallback error_callback = nullptr);
    
    /**
     * 停止生产者线程
     * 
     * 阻塞等待线程安全退出
     */
    void stopVideoProducer();
    
    /**
     * 获取生产者线程状态
     * @return 当前状态
     */
    ProducerState getProducerState() const;
    
    /**
     * 获取最后一次错误信息
     * @return 错误消息字符串
     */
    std::string getLastProducerError() const;
    
    /**
     * 检查生产者线程是否正在运行
     * @return 运行中返回true
     */
    bool isProducerRunning() const;
    
private:
    // ============ Buffer 管理 ============
    std::vector<Buffer> buffers_;        // Buffer 对象数组
    std::vector<void*> memory_blocks_;   // 物理内存块指针（用于释放）
    std::vector<int> dma_fds_;           // DMA buffer 文件描述符（CMA模式）
    
    size_t buffer_size_;                 // 每个 Buffer 的大小
    bool use_cma_;                       // 是否使用 CMA
    
    // ============ 队列管理 ============
    std::queue<Buffer*> free_queue_;     // 空闲队列
    std::queue<Buffer*> filled_queue_;   // 就绪队列
    
    // ============ 线程同步 ============
    mutable std::mutex mutex_;           // 互斥锁
    std::condition_variable free_cv_;    // 空闲队列条件变量
    std::condition_variable filled_cv_;  // 就绪队列条件变量
    
    // ============ 生产者线程管理（统一使用 vector 支持单/多线程）============
    std::vector<std::thread> producer_threads_;      // 生产者线程数组（支持1个或多个线程）
    std::atomic<bool> producer_running_;             // 线程运行标志（所有线程共享）
    std::atomic<ProducerState> producer_state_;      // 线程状态（所有线程共享）
    int producer_thread_count_;                      // 当前运行的生产者线程数量
    
    // 多线程协调（用于多生产者模式）
    std::atomic<int> next_frame_index_;              // 下一个要读取的帧索引
    std::mutex video_file_mutex_;                    // VideoFile 互斥锁（如果共享）
    
    // io_uring readers管理（用于io_uring模式）
    std::vector<void*> iouring_readers_;             // IoUringVideoReader指针数组（避免头文件依赖，使用void*）
    std::shared_ptr<VideoFile> shared_video_file_;   // 共享的VideoFile对象（多线程共享）
    
    // 错误处理
    ErrorCallback error_callback_;                   // 错误回调函数
    std::string last_error_;                         // 最后一次错误消息
    mutable std::mutex error_mutex_;                 // 错误信息互斥锁
    
    // ============ 内部方法 ============
    
    /**
     * 分配物理连续内存（CMA）
     * @param size 内存大小
     * @param out_fd 输出 DMA buffer 文件描述符
     * @return 内存地址，失败返回 nullptr
     */
    void* allocateCMAMemory(size_t size, int& out_fd);
    
    /**
     * 分配普通内存
     * @param size 内存大小
     * @return 内存地址，失败返回 nullptr
     */
    void* allocateNormalMemory(size_t size);
    
    /**
     * 释放 CMA 内存
     * @param addr 内存地址
     * @param size 内存大小
     * @param fd DMA buffer 文件描述符
     */
    void freeCMAMemory(void* addr, size_t size, int fd);
    
    /**
     * 释放普通内存
     * @param addr 内存地址
     */
    void freeNormalMemory(void* addr);
    
    /**
     * 统一的生产者启动内部实现
     * @param thread_count 线程数量（1表示单线程，>1表示多线程）
     * @param video_file_path 视频文件路径
     * @param width 视频宽度
     * @param height 视频高度
     * @param bits_per_pixel 每像素位数
     * @param loop 是否循环播放
     * @param error_callback 错误回调
     * @return 成功返回true
     */
    bool startVideoProducerInternal(int thread_count,
                                   const char* video_file_path, 
                                   int width, int height, int bits_per_pixel,
                                   bool loop,
                                   ErrorCallback error_callback);
    
    /**
     * 视频文件生产者线程函数（单线程模式）
     * @param video_file_path 视频文件路径
     * @param width 视频宽度
     * @param height 视频高度
     * @param bits_per_pixel 每像素位数
     * @param loop 是否循环播放
     */
    void videoProducerThread(const char* video_file_path, 
                            int width, int height, int bits_per_pixel,
                            bool loop);
    
    /**
     * 视频文件生产者线程函数（多线程模式）
     * @param thread_id 线程ID（0, 1, 2...）
     * @param shared_video 共享的VideoFile对象
     * @param loop 是否循环播放
     * @param total_frames 视频总帧数
     */
    void multiVideoProducerThread(int thread_id,
                                 std::shared_ptr<VideoFile> shared_video,
                                 bool loop, int total_frames);
    
    /**
     * 设置错误信息并触发回调
     * @param error_msg 错误消息
     */
    void setError(const std::string& error_msg);
};

#endif // BUFFER_MANAGER_HPP

