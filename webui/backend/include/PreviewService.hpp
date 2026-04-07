#ifndef WEBUI_PREVIEW_SERVICE_HPP
#define WEBUI_PREVIEW_SERVICE_HPP

#include "ApiTypes.hpp"
#include <mutex>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <functional>

namespace webui {

class WorkerManager;
class ConsumerManager;

class PreviewService {
public:
    PreviewService(WorkerManager& wk_mgr, ConsumerManager& cs_mgr);
    ~PreviewService() = default;

    // MJPEG 流输出回调: 调用者传入写回调, 服务持续推送 JPEG 帧
    using FrameCallback = std::function<bool(const uint8_t* data, size_t len)>;
    void streamMjpeg(const std::string& worker_id, FrameCallback cb);

    // 单帧截图
    std::vector<uint8_t> snapshot(const std::string& worker_id, int quality = 80);

    // 获取多路预览布局信息
    ApiResponse gridInfo(const std::string& layout) const;

    // 由 encode worker 回调注入 JPEG 帧
    void onJpegFrame(const std::string& worker_id, const uint8_t* data, size_t len);

private:
    struct FrameBuffer {
        std::mutex mutex;
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
