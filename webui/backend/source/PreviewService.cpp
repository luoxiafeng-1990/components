#include "../include/PreviewService.hpp"
#include "../include/WorkerManager.hpp"
#include "../include/ConsumerManager.hpp"
#include "consumptionline/types/stitcher/FrameStitcherService.hpp"
#include <thread>
#include <chrono>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

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

    // Stop composite encoder thread
    encoder_running_ = false;
    composite_raw_cv_.notify_all();
    if (composite_encoder_thread_.joinable()) {
        composite_encoder_thread_.join();
    }

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

// ============================================================
// Composite preview (stitched multi-channel)
// ============================================================

void PreviewService::connectStitcher(std::shared_ptr<FrameStitcherService> stitcher) {
    if (!stitcher) return;
    stitcher_ = stitcher;

    // Subscribe to stitched frames
    stitcher_->subscribe([this](const StitchedFrame& frame) {
        // Fast path: memcpy raw NV12 data for async encoding
        if (!encoder_running_) return;

        std::lock_guard<std::mutex> lock(composite_raw_mutex_);
        if (composite_raw_buf_.size() != frame.data_size) {
            composite_raw_buf_.resize(frame.data_size);
        }

        void* src = frame.buffer->getVirtualAddress();
        if (src) {
            memcpy(composite_raw_buf_.data(), src, frame.data_size);
            composite_width_ = frame.width;
            composite_height_ = frame.height;
            composite_raw_ready_ = true;
            composite_raw_cv_.notify_one();
        }
    });

    // Start encoder thread
    encoder_running_ = true;
    composite_encoder_thread_ = std::thread(&PreviewService::compositeEncoderThreadFunc, this);
    composite_available_ = true;
}

void PreviewService::compositeEncoderThreadFunc() {
    while (encoder_running_ && !stop_requested_) {
        // Wait for a new raw frame
        {
            std::unique_lock<std::mutex> lock(composite_raw_mutex_);
            composite_raw_cv_.wait_for(lock, std::chrono::milliseconds(200), [this] {
                return composite_raw_ready_.load() || !encoder_running_ || stop_requested_.load();
            });
            if (!encoder_running_ || stop_requested_) break;
            if (!composite_raw_ready_) continue;
            composite_raw_ready_ = false;
        }

        int w, h;
        std::vector<uint8_t> nv12_copy;
        {
            std::lock_guard<std::mutex> lock(composite_raw_mutex_);
            w = composite_width_;
            h = composite_height_;
            nv12_copy = composite_raw_buf_;
        }
        if (w <= 0 || h <= 0 || nv12_copy.empty()) continue;

        // NV12 → BGR via OpenCV
        cv::Mat nv12_mat(h * 3 / 2, w, CV_8UC1, nv12_copy.data());
        cv::Mat bgr_mat;
        cv::cvtColor(nv12_mat, bgr_mat, cv::COLOR_YUV2BGR_NV12);

        // BGR → JPEG
        std::vector<uint8_t> jpeg_buf;
        std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 70};
        cv::imencode(".jpg", bgr_mat, jpeg_buf, params);

        if (!jpeg_buf.empty()) {
            // Use the existing FrameBuffer mechanism via "__composite__" key
            onJpegFrame("__composite__", jpeg_buf.data(), jpeg_buf.size());
        }
    }
}

void PreviewService::streamCompositeMjpeg(FrameCallback cb) {
    streamMjpeg("__composite__", cb);
}

std::vector<uint8_t> PreviewService::compositeSnapshot() {
    return snapshot("__composite__");
}

} // namespace webui
