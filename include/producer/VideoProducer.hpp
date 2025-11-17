#pragma once

#include "../buffer/BufferPool.hpp"
#include "../videoFile/VideoFile.hpp"
#include "../monitor/PerformanceMonitor.hpp"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <functional>

/**
 * @brief VideoProducer - 独立的视频生产者模块
 * 
 * 职责：
 * - 从视频文件读取帧数据
 * - 填充 BufferPool 提供的 buffer
 * - 管理多个生产者线程
 * - 性能监控和统计
 * 
 * 设计特点：
 * - 依赖注入 BufferPool（通过引用持有，不拥有所有权）
 * - 职责单一（只负责视频读取）
 * - 配置驱动（通过 Config 结构体）
 * - 线程安全（支持多线程生产）
 */
class VideoProducer {
public:
    /**
     * @brief 视频配置结构
     */
    struct Config {
        std::string file_path;                         // 视频文件路径
        int width;                                     // 分辨率宽度
        int height;                                    // 分辨率高度
        int bits_per_pixel;                            // 每像素位数（8/16/24/32）
        bool loop;                                     // 是否循环播放
        int thread_count;                              // 生产者线程数（默认1）
        VideoReaderFactory::ReaderType reader_type;    // 读取器类型（默认AUTO）
        
        // 默认构造
        Config() 
            : width(0), height(0), bits_per_pixel(0)
            , loop(false), thread_count(1)
            , reader_type(VideoReaderFactory::ReaderType::AUTO) {}
        
        // 便利构造
        Config(const std::string& path, int w, int h, int bpp, bool l = false, int tc = 1,
               VideoReaderFactory::ReaderType rt = VideoReaderFactory::ReaderType::AUTO)
            : file_path(path), width(w), height(h), bits_per_pixel(bpp)
            , loop(l), thread_count(tc), reader_type(rt) {}
    };
    
    /**
     * @brief 错误回调函数类型
     */
    using ErrorCallback = std::function<void(const std::string&)>;
    
    /**
     * @brief 构造函数（依赖注入）
     * @param pool BufferPool 引用（不拥有所有权）
     */
    explicit VideoProducer(BufferPool& pool);
    
    /**
     * @brief 析构函数 - 自动停止生产者
     */
    ~VideoProducer();
    
    // 禁止拷贝和赋值
    VideoProducer(const VideoProducer&) = delete;
    VideoProducer& operator=(const VideoProducer&) = delete;
    
    // ========== 核心接口 ==========
    
    /**
     * @brief 启动视频生产者
     * @param config 视频配置
     * @return true 如果启动成功
     */
    bool start(const Config& config);
    
    /**
     * @brief 停止视频生产者
     */
    void stop();
    
    // ========== 查询接口 ==========
    
    /// 是否正在运行
    bool isRunning() const { return running_.load(); }
    
    /// 获取已生产的帧数
    int getProducedFrames() const { return produced_frames_.load(); }
    
    /// 获取跳过的帧数（读取失败）
    int getSkippedFrames() const { return skipped_frames_.load(); }
    
    /// 获取平均 FPS
    double getAverageFPS() const;
    
    /// 获取总帧数
    int getTotalFrames() const;
    
    // ========== 错误处理 ==========
    
    /**
     * @brief 设置错误回调
     * @param callback 错误回调函数
     */
    void setErrorCallback(ErrorCallback callback) {
        error_callback_ = callback;
    }
    
    /**
     * @brief 获取最后一次错误信息
     */
    std::string getLastError() const;
    
    // ========== 调试接口 ==========
    
    /// 打印统计信息
    void printStats() const;
    
private:
    // ========== 内部方法 ==========
    
    /**
     * @brief 生产者线程函数
     * @param thread_id 线程ID
     */
    void producerThreadFunc(int thread_id);
    
    /**
     * @brief 设置错误信息并触发回调
     */
    void setError(const std::string& error_msg);
    
    // ========== 成员变量 ==========
    
    // BufferPool 引用（依赖注入，不拥有所有权）
    BufferPool& buffer_pool_;
    
    // 🆕 工作 BufferPool 指针（可能指向 buffer_pool_ 或 Reader 内部的 BufferPool）
    BufferPool* buffer_pool_ptr_;
    
    // 视频文件（多线程共享）
    std::shared_ptr<VideoFile> video_file_;
    
    // 线程管理
    std::vector<std::thread> threads_;
    std::atomic<bool> running_;
    
    // 统计信息
    std::atomic<int> produced_frames_;
    std::atomic<int> skipped_frames_;
    std::atomic<int> next_frame_index_;  // 下一个要读取的帧索引（原子递增）
    
    // 配置
    Config config_;
    int total_frames_;
    
    // 错误处理
    ErrorCallback error_callback_;
    mutable std::mutex error_mutex_;
    std::string last_error_;
    
    // 性能监控
    std::chrono::steady_clock::time_point start_time_;
};


