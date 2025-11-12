#ifndef IOURING_VIDEO_READER_HPP
#define IOURING_VIDEO_READER_HPP

#include "../buffer/Buffer.hpp"
#include "../buffer/BufferManager.hpp"
#include <liburing.h>
#include <string>
#include <vector>
#include <atomic>

/**
 * IoUringVideoReader - 基于io_uring的高性能视频文件读取器
 * 
 * 特点：
 * - 零拷贝异步I/O
 * - 批量提交读取请求
 * - 显著降低CPU使用率
 * - 提高I/O吞吐量
 * 
 * 使用场景：
 * - 多线程并发读取视频帧
 * - 随机访问模式
 * - 性能敏感的应用
 */
class IoUringVideoReader {
public:
    /**
     * 构造函数
     * 
     * @param video_path 视频文件路径
     * @param width 视频宽度
     * @param height 视频高度
     * @param bits_per_pixel 每像素位数
     * @param queue_depth io_uring队列深度（建议16-64）
     */
    IoUringVideoReader(const char* video_path, 
                      int width, int height, int bits_per_pixel,
                      int queue_depth = 32);
    
    /**
     * 析构函数
     */
    ~IoUringVideoReader();
    
    // 禁止拷贝
    IoUringVideoReader(const IoUringVideoReader&) = delete;
    IoUringVideoReader& operator=(const IoUringVideoReader&) = delete;
    
    /**
     * 检查是否初始化成功
     */
    bool isInitialized() const { return initialized_; }
    
    /**
     * 获取总帧数
     */
    int getTotalFrames() const { return total_frames_; }
    
    /**
     * 获取帧大小
     */
    size_t getFrameSize() const { return frame_size_; }
    
    /**
     * 异步生产者线程（用于替代multiVideoProducerThread）
     * 
     * @param thread_id 线程ID
     * @param manager BufferManager指针
     * @param frame_indices 要读取的帧索引列表
     * @param running 运行标志
     * @param loop 是否循环读取
     */
    void asyncProducerThread(int thread_id,
                            BufferManager* manager,
                            const std::vector<int>& frame_indices,
                            std::atomic<bool>& running,
                            bool loop = false);
    
    /**
     * 提交批量读取请求
     * 
     * @param manager BufferManager指针
     * @param frame_indices 要读取的帧索引
     * @return 成功提交的请求数量
     */
    int submitReadBatch(BufferManager* manager, 
                       const std::vector<int>& frame_indices);
    
    /**
     * 收割完成的I/O请求
     * 
     * @param manager BufferManager指针
     * @param blocking 是否阻塞等待
     * @return 完成的请求数量
     */
    int harvestCompletions(BufferManager* manager, bool blocking = false);
    
    /**
     * 获取统计信息
     */
    struct Stats {
        long total_reads;         // 总读取次数
        long successful_reads;    // 成功读取次数
        long failed_reads;        // 失败读取次数
        long total_bytes;         // 总字节数
        double avg_latency_us;    // 平均延迟（微秒）
    };
    
    Stats getStats() const;
    void resetStats();

private:
    // io_uring相关
    struct io_uring ring_;
    int queue_depth_;
    bool initialized_;
    
    // 文件相关
    int video_fd_;
    std::string video_path_;
    size_t frame_size_;
    int total_frames_;
    int width_;
    int height_;
    int bits_per_pixel_;
    
    // 用于追踪每个I/O请求
    struct ReadRequest {
        Buffer* buffer;           // 目标buffer
        int frame_index;          // 帧索引
        BufferManager* manager;   // BufferManager指针
        std::chrono::high_resolution_clock::time_point start_time;  // 开始时间
    };
    
    // 统计信息
    std::atomic<long> total_reads_{0};
    std::atomic<long> successful_reads_{0};
    std::atomic<long> failed_reads_{0};
    std::atomic<long> total_bytes_{0};
    std::atomic<long> total_latency_us_{0};
};

#endif // IOURING_VIDEO_READER_HPP


