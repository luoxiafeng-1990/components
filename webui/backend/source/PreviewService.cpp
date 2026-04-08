#include "../include/PreviewService.hpp"
#include "../include/WorkerManager.hpp"
#include "../include/ConsumerManager.hpp"
#include <thread>
#include <chrono>

namespace webui {

PreviewService::PreviewService(WorkerManager& wk_mgr, ConsumerManager& cs_mgr)
    : worker_manager_(wk_mgr), consumer_manager_(cs_mgr)
{
}

PreviewService::FrameBuffer& PreviewService::getOrCreateBuffer(const std::string& worker_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = frame_buffers_.find(worker_id);
    if (it == frame_buffers_.end()) {
        frame_buffers_[worker_id] = std::make_unique<FrameBuffer>();
        return *frame_buffers_[worker_id];
    }
    return *it->second;
}

void PreviewService::onJpegFrame(const std::string& worker_id,
                                  const uint8_t* data, size_t len)
{
    auto& fb = getOrCreateBuffer(worker_id);
    std::lock_guard<std::mutex> lock(fb.mutex);

    // 更新 latest（snapshot 用）
    fb.latest_frame.assign(data, data + len);
    fb.frame_seq++;

    // 入队（MJPEG stream 用），队满丢最旧帧
    fb.frame_queue.emplace_back(data, data + len);
    while (fb.frame_queue.size() > MAX_QUEUE_SIZE) {
        fb.frame_queue.pop_front();
    }

    fb.cv.notify_all();
}

void PreviewService::requestStop() {
    stop_requested_ = true;
    // 唤醒所有等待中的 streamMjpeg
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [_, fb] : frame_buffers_) {
        fb->cv.notify_all();
    }
}

void PreviewService::streamMjpeg(const std::string& worker_id, FrameCallback cb) {
    auto& fb = getOrCreateBuffer(worker_id);

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        std::vector<uint8_t> frame;
        {
            std::unique_lock<std::mutex> lock(fb.mutex);
            // 等待队列中有帧或停止
            fb.cv.wait_for(lock, std::chrono::milliseconds(200), [&] {
                return !fb.frame_queue.empty() || stop_requested_.load(std::memory_order_relaxed);
            });

            if (stop_requested_.load(std::memory_order_relaxed)) break;
            if (fb.frame_queue.empty()) continue;

            frame = std::move(fb.frame_queue.front());
            fb.frame_queue.pop_front();
        }

        if (!frame.empty()) {
            if (!cb(frame.data(), frame.size())) {
                break;
            }
        }

        // 按目标帧率节流
        int fps = target_fps_.load();
        if (fps > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000 / fps));
        }
    }
}

std::vector<uint8_t> PreviewService::snapshot(const std::string& worker_id, int /*quality*/) {
    auto& fb = getOrCreateBuffer(worker_id);
    std::lock_guard<std::mutex> lock(fb.mutex);
    return fb.latest_frame;
}

ApiResponse PreviewService::gridInfo(const std::string& layout) const {
    int cols = 3, rows = 3;
    if (layout == "2x2") { cols = 2; rows = 2; }
    else if (layout == "4x4") { cols = 4; rows = 4; }

    auto workers_resp = worker_manager_.list();
    json streams = json::array();
    int slot = 0;

    if (workers_resp.data.is_array()) {
        for (auto& w : workers_resp.data) {
            if (slot >= cols * rows) break;
            std::string wid = w.value("id", "");
            if (consumer_manager_.hasJpegPreview(wid)) {
                streams.push_back({
                    {"slot", slot},
                    {"worker_id", wid},
                    {"worker_name", w.value("name", "")},
                    {"stream_url", "/api/preview/stream/" + wid},
                    {"state", w.value("state", "STOPPED")}
                });
                slot++;
            }
        }
    }

    return ApiResponse::ok({
        {"layout", layout},
        {"total_slots", cols * rows},
        {"streams", streams}
    });
}

} // namespace webui
