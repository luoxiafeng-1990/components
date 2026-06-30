#ifndef OPENCV_WORKER_HPP
#define OPENCV_WORKER_HPP

#include "productionline/worker/base/WorkerBase.hpp"
#include "productionline/worker/datasource/encodeddata/IEncodedPacketSource.hpp"
#include "common/StageTimer.hpp"
#include "bufferpool/buffer/Buffer.hpp"
#include "bufferpool/pool/base/BufferPool.hpp"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <memory>
#include <array>

#include "opencv2/core.hpp"
#include "opencv2/core/tacv.hpp"
#include "opencv2/imgproc.hpp"
#include "opencv2/imgcodecs.hpp"

class OpencvWorker : public WorkerBase {
public:
    explicit OpencvWorker(const WorkerConfig& config);
    virtual ~OpencvWorker();
    
    // 禁止拷贝
    OpencvWorker(const OpencvWorker&) = delete;
    OpencvWorker& operator=(const OpencvWorker&) = delete;
    
    FillResult fillBuffer(int frame_index, Buffer* buffer) override;
    const char* getWorkerType() const override {
        return "OpencvWorker";
    }
    
    bool open() override;
    bool open(const char* path) override;
    
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
    std::string getPath() const override;
    bool hasMoreFrames() const override;
    SourceType getDataSourceType() const override;
    bool isAtEnd() const override;
    
    bool setSourceBufferPool(std::weak_ptr<BufferPool> pool_weak) override;
    
    /**
     * @brief 获取编解码器参数（用于 BufferWriter 等场景）
     * @return 编解码器参数指针，如果不可用则返回 nullptr
     */
    const AVCodecParameters* getCodecParameters() const override;
    
    int getSourceWidth() const override;
    int getSourceHeight() const override;
    AVPixelFormat getSourcePixelFormat() const override;
    
    /**
     * @brief 获取 Worker 输出的视频宽度
     */
    int getOutputWidth() const override;
    
    /**
     * @brief 获取 Worker 输出的视频高度
     */
    int getOutputHeight() const override;
    
    /**
     * @brief 获取 Worker 输出的每像素字节数
     * @param channel 通道编号（默认 0）
     */
    double getOutputBytesPerPixel(int channel = 0) const override;

    BufferPoolType getPrimaryBufferPoolType() const override;

    cv::Mat mockMat(int width, int height, bool hw, AVPixelFormat pix_fmt);

private:
    // ============ Logger ============
    log4cplus::Logger logger;
    std::string file_path;
    std::string file_list_[128];
    size_t file_num;
    int current_file_index;
    bool use_hardware;
    bool use_mock;
    int src_height;
    int src_width;
    AVPixelFormat pix_fmt;
    
    // ============ 线程安全 ============
    mutable std::recursive_mutex mutex_;  // 使用递归锁避免死锁   
    perf::StageTimer imread_timer_{"opencv_imread"};
};

#endif // OPENCV_WORKER_HPP

