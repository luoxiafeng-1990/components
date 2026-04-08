#ifndef WEBUI_PREVIEW_SERVICE_HPP
#define WEBUI_PREVIEW_SERVICE_HPP

#include "ApiTypes.hpp"
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <vector>
#include <deque>
#include <atomic>
#include <functional>

namespace webui {

class WorkerManager;
class ConsumerManager;

class PreviewService {
public:
    PreviewService(WorkerManager& wk_mgr, ConsumerManager& cs_mgr);
    ~PreviewService() = default;

    using FrameCallback = std::function<bool(const uint8_t* data, size_t len)>;

    /// MJPEG 流：按 target_fps 均匀推送，阻塞式等待新帧
    void streamMjpeg(const std::string& worker_id, FrameCallback cb);

    /// 单帧截图：取最新帧
    std::vector<uint8_t> snapshot(const std::string& worker_id, int quality = 80);

    /// 布局信息
    ApiResponse gridInfo(const std::string& layout) const;

    /// 编码回调注入帧
    void onJpegFrame(const std::string& worker_id, const uint8_t* data, size_t len);

    void requestStop();

    /// 设置 MJPEG 流的目标帧率（全局）
    void setTargetFps(int fps) { target_fps_ = fps > 0 ? fps : 15; }
    int getTargetFps() const { return target_fps_; }

private:
    std::atomic<bool> stop_requested_{false};
    std::atomic<int> target_fps_{15};

    static constexpr size_t MAX_QUEUE_SIZE = 8;

    struct FrameBuffer {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<std::vector<uint8_t>> frame_queue;
        std::vector<uint8_t> latest_frame;
        std::atomic<uint64_t> frame_seq{0};
    };

    FrameBuffer& getOrCreateBuffer(const std::string& worker_id);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<FrameBuffer>> frame_buffers_;
    WorkerManager& worker_manager_;
    ConsumerManager& consumer_manager_;
};

} // namespace webui

#endif // WEBUI_PREVIEW_SERVICE_HPP
