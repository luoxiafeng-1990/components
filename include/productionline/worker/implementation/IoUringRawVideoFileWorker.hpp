#ifndef IOURING_RAW_VIDEO_FILE_WORKER_HPP
#define IOURING_RAW_VIDEO_FILE_WORKER_HPP

#include "../base/WorkerBase.hpp"
#include "../../../buffer/Buffer.hpp"
#include "../../../buffer/BufferPool.hpp"
#include <liburing.h>
#include <string>
#include <vector>
#include <atomic>

/**
 * @brief IoUringRawVideoFileWorker - IoUring方式打开raw视频文件Worker
 * 
 * 架构角色：Worker（工人）- IoUring方式打开raw视频文件类型
 * 
 * 功能：使用io_uring高性能异步I/O方式打开raw视频文件
 * 目的：填充Buffer，得到填充后的buffer
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
 * - 大文件（>1GB）高并发读取
 */
class IoUringRawVideoFileWorker : public WorkerBase {
public:
    /**
     * 构造函数（默认）
     * @param queue_depth io_uring队列深度（建议16-64）
     */
    IoUringRawVideoFileWorker(int queue_depth = 32);
    
    /**
     * 析构函数
     */
    virtual ~IoUringRawVideoFileWorker();
    
    // 禁止拷贝
    IoUringRawVideoFileWorker(const IoUringRawVideoFileWorker&) = delete;
    IoUringRawVideoFileWorker& operator=(const IoUringRawVideoFileWorker&) = delete;
    
    // ============ WorkerBase 接口实现 ============
    
    // Buffer填充功能（原IBufferFillingWorker的方法）
    bool fillBuffer(int frame_index, Buffer* buffer) override;
    const char* getWorkerType() const override {
        return "IoUringRawVideoFileWorker";
    }
    
    // 文件导航功能（继承自IVideoFileNavigator）
    bool open(const char* path) override;
    bool open(const char* path, int width, int height, int bits_per_pixel) override;
    void close() override;
    bool isOpen() const override;
    bool seek(int frame_index) override;
    bool seekToBegin() override;
    bool seekToEnd() override;
    bool skip(int frame_count) override;
    int getTotalFrames() const override;
    int getCurrentFrameIndex() const override;
    size_t getFrameSize() const override;
    long getFileSize() const override;
    int getWidth() const override;
    int getHeight() const override;
    int getBytesPerPixel() const override;
    const char* getPath() const override;
    bool hasMoreFrames() const override;
    bool isAtEnd() const override;
    
    // ============ IoUring 专有接口（保留原有功能） ============
    
    /**
     * 异步生产者线程（用于替代multiVideoProducerThread）
     * 
     * @param thread_id 线程ID
     * @param pool BufferPool指针
     * @param frame_indices 要读取的帧索引列表
     * @param running 运行标志
     * @param loop 是否循环读取
     */
    void asyncProducerThread(int thread_id,
                            BufferPool* pool,
                            const std::vector<int>& frame_indices,
                            std::atomic<bool>& running,
                            bool loop = false);
    
    /**
     * 提交批量读取请求
     * 
     * @param pool BufferPool指针
     * @param frame_indices 要读取的帧索引
     * @return 成功提交的请求数
     */
    int submitBatchReads(BufferPool* pool, const std::vector<int>& frame_indices);

private:
    // ============ io_uring 资源 ============
    struct io_uring ring_;
    int queue_depth_;
    bool initialized_;
    
    // ============ 文件资源 ============
    int video_fd_;
    std::string video_path_;
    
    // ============ 视频属性 ============
    size_t frame_size_;
    long file_size_;
    int total_frames_;
    int current_frame_index_;
    int width_;
    int height_;
    int bits_per_pixel_;
    
    // ============ 状态 ============
    bool is_open_;
    
    // ============ 内部辅助方法 ============
    
    /**
     * 初始化io_uring
     */
    bool initializeIoUring();
    
    /**
     * 清理io_uring资源
     */
    void cleanupIoUring();
    
    /**
     * 提交单个读取请求
     */
    bool submitReadRequest(int frame_index, void* buffer, size_t buffer_size);
    
    /**
     * 等待并完成读取请求
     */
    bool waitForCompletion();
};

#endif // IOURING_RAW_VIDEO_FILE_WORKER_HPP

