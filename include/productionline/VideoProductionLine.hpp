#pragma once

#include "../buffer/BufferPool.hpp"
#include "worker/facade/BufferFillingWorkerFacade.hpp"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <functional>

/**
 * @brief VideoProductionLine - 视频生产流水线
 * 
 * 架构角色：ProductionLine（生产流水线）- 从Worker获取原材料（BufferPool），进行生产
 * 
 * 职责：
 * - 从Worker获取BufferPool（原材料）
 * - 填充 BufferPool 提供的 buffer
 * - 管理多个生产者线程
 * - 性能监控和统计
 * 
 * 设计特点：
 * - Worker必须创建BufferPool（通过调用Allocator）
 * - 从Worker获取BufferPool（原材料）
 * - 职责单一（只负责视频读取和生产）
 * - 配置驱动（通过 Config 结构体）
 * - 线程安全（支持多线程生产）
 */
class VideoProductionLine {
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
        BufferFillingWorkerFactory::WorkerType worker_type;    // Worker类型（默认AUTO）
        
        // 🎯 FFmpeg解码器配置（用于 FFMPEG_VIDEO_FILE 类型）
        const char* decoder_name;                      // 解码器名称（如 "h264_taco"，nullptr=自动选择）
        
        // 默认构造
        Config() 
            : width(0), height(0), bits_per_pixel(0)
            , loop(false), thread_count(1)
            , worker_type(BufferFillingWorkerFactory::WorkerType::AUTO)
            , decoder_name(nullptr) {}  // 🎯 默认自动选择解码器
        
        // 便利构造
        Config(const std::string& path, int w, int h, int bpp, bool l = false, int tc = 1,
               BufferFillingWorkerFactory::WorkerType wt = BufferFillingWorkerFactory::WorkerType::AUTO)
            : file_path(path), width(w), height(h), bits_per_pixel(bpp)
            , loop(l), thread_count(tc), worker_type(wt)
            , decoder_name(nullptr) {}  // 🎯 默认自动选择解码器
    };
    
    /**
     * @brief 错误回调函数类型
     */
    using ErrorCallback = std::function<void(const std::string&)>;
    
    /**
     * @brief 构造函数
     * 
     * 注意：Worker必须在open()时自动创建BufferPool（通过调用Allocator）
     * ProductionLine从Worker获取BufferPool，不再需要外部注入
     */
    VideoProductionLine();
    
    /**
     * @brief 析构函数 - 自动停止生产者
     */
    ~VideoProductionLine();
    
    // 禁止拷贝和赋值
    VideoProductionLine(const VideoProductionLine&) = delete;
    VideoProductionLine& operator=(const VideoProductionLine&) = delete;
    
    // ========== 核心接口 ==========
    
    /**
     * @brief 启动视频生产流水线
     * @param config 视频配置
     * @return true 如果启动成功
     */
    bool start(const Config& config);
    
    /**
     * @brief 停止视频生产流水线
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
    
    /// 获取工作BufferPool指针（供消费者使用）
    /// v2.0: 从Registry获取临时访问
    BufferPool* getWorkingBufferPool() const;
    
    /// v2.0: 获取工作BufferPool ID
    uint64_t getWorkingBufferPoolId() const { return working_buffer_pool_id_; }
    
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
    
    /**
     * v2.0: Worker创建的BufferPool ID（Registry持有）
     * 
     * 用途：记录Worker创建的BufferPool的ID
     * 
     * 注意：Worker必须在open()时自动创建BufferPool（通过调用Allocator）
     * 如果Worker没有创建BufferPool，start()会失败
     * 
     * Registry独占持有BufferPool（shared_ptr，引用计数=1）
     * ProductionLine从Registry获取临时访问
     */
    uint64_t working_buffer_pool_id_;
    
    /**
     * v2.0: 实际工作的BufferPool指针（缓存的临时访问）
     * 
     * 警告：此指针在ProductionLine运行期间有效
     * 如果Pool被销毁（Allocator析构），此指针会失效
     */
    BufferPool* working_buffer_pool_ptr_;
    
    // Worker Facade（多线程共享）
    std::shared_ptr<BufferFillingWorkerFacade> worker_facade_sptr_;
    
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

