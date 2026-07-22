#include "../include/PreviewService.hpp"
#include "../include/WorkerManager.hpp"
#include "../include/ConsumerManager.hpp"
#include "../include/PreviewSessionManager.hpp"
#include "consumptionline/types/stitcher/FrameStitcherService.hpp"
#include <thread>
#include <chrono>
#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

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

    // 帧率统计
    auto now = std::chrono::steady_clock::now();
    fb.fps_frame_count++;
    auto elapsed = std::chrono::duration<double>(now - fb.fps_last_calc).count();
    if (elapsed >= 1.0) {
        fb.current_fps = fb.fps_frame_count / elapsed;
        fb.fps_frame_count = 0;
        fb.fps_last_calc = now;
    }

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
    // Session path: shared latest_jpeg broadcast (spec §8.4). Clients never
    // clear the shared queue — each tracks last_sent_sequence independently.
    if (session_manager_ && session_manager_->hasActiveSession(worker_id)) {
        uint64_t last_sent_sequence = 0;
        auto last_send = std::chrono::steady_clock::now();

        while (!stop_requested_.load(std::memory_order_relaxed)) {
            if (!session_manager_->hasActiveSession(worker_id)) {
                break; // fall through to legacy only on next request
            }

            int fps = target_fps_.load();
            int wait_ms = (fps > 0) ? (1000 / fps) : 40;

            PreviewJpegSnapshot snap;
            if (!session_manager_->waitLatestJpeg(
                    worker_id, last_sent_sequence, snap, wait_ms)) {
                if (stop_requested_.load(std::memory_order_relaxed)) {
                    break;
                }
                continue;
            }

            if (!snap.data || snap.data->empty()) {
                continue;
            }

            auto now = std::chrono::steady_clock::now();
            int min_interval_ms = (fps > 0) ? (1000 / fps) : 40;
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               now - last_send)
                               .count();
            if (elapsed < min_interval_ms) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(min_interval_ms - elapsed));
            }

            last_sent_sequence = snap.sequence;
            if (!cb(snap.data->data(), snap.data->size())) {
                break;
            }
            last_send = std::chrono::steady_clock::now();
        }
        return;
    }

    // Legacy QA / static JpegEncodeConsumer path (onJpegFrame → frame_queue).
    auto& fb = getOrCreateBuffer(worker_id);

    auto last_send = std::chrono::steady_clock::now();

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        std::vector<uint8_t> frame;
        {
            std::unique_lock<std::mutex> lock(fb.mutex);
            int fps = target_fps_.load();
            int wait_ms = (fps > 0) ? (1000 / fps) : 40;

            fb.cv.wait_for(lock, std::chrono::milliseconds(wait_ms), [&] {
                return !fb.frame_queue.empty() || stop_requested_.load(std::memory_order_relaxed);
            });

            if (stop_requested_.load(std::memory_order_relaxed)) break;
            if (fb.frame_queue.empty()) continue;

            // Take the newest frame, discard older ones to reduce latency
            frame = std::move(fb.frame_queue.back());
            fb.frame_queue.clear();
        }

        if (!frame.empty()) {
            auto now = std::chrono::steady_clock::now();
            int fps = target_fps_.load();
            int min_interval_ms = (fps > 0) ? (1000 / fps) : 40;
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_send).count();

            if (elapsed < min_interval_ms) {
                std::this_thread::sleep_for(std::chrono::milliseconds(min_interval_ms - elapsed));
            }

            if (!cb(frame.data(), frame.size())) {
                break;
            }
            last_send = std::chrono::steady_clock::now();
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

ApiResponse PreviewService::getLayout() {
    // Prefer live stitcher instance (source of truth for IDS grid geometry).
    auto stitcher = FrameStitcherService::getInstance();
    if (!stitcher) {
        stitcher = stitcher_;
    }

    if (!stitcher) {
        return ApiResponse::ok({
            {"width", 0},
            {"height", 0},
            {"rows", 0},
            {"cols", 0},
            {"view_type", "grid"},
            {"slots", json::array()}
        });
    }

    const LayoutSnapshot snap = stitcher->getLayoutSnapshot();

    // Index workers by id for name/state merge (does not define slot order).
    std::unordered_map<std::string, json> workers_by_id;
    auto workers_resp = worker_manager_.list();
    if (workers_resp.data.is_array()) {
        for (const auto& w : workers_resp.data) {
            const std::string wid = w.value("id", "");
            if (!wid.empty()) {
                workers_by_id[wid] = w;
            }
        }
    }

    json slots = json::array();
    for (const auto& s : snap.slots) {
        json slot = {
            {"slot", s.slot},
            {"channel_id", s.channel_id},
            {"worker_id", s.worker_id},
            {"worker_name", ""},
            {"x", s.x},
            {"y", s.y},
            {"width", s.width},
            {"height", s.height},
            {"state", json(nullptr)}
        };

        if (!s.worker_id.empty()) {
            auto it = workers_by_id.find(s.worker_id);
            if (it != workers_by_id.end()) {
                slot["worker_name"] = it->second.value("name", "");
                slot["state"] = it->second.value("state", "STOPPED");
            }
        }

        slots.push_back(std::move(slot));
    }

    return ApiResponse::ok({
        {"width", snap.width},
        {"height", snap.height},
        {"rows", snap.rows},
        {"cols", snap.cols},
        {"view_type", snap.view_type},
        {"slots", slots}
    });
}

// ============================================================
// Composite preview (stitched multi-channel)
// ============================================================

void PreviewService::connectStitcher(std::shared_ptr<FrameStitcherService> stitcher) {
    if (!stitcher) return;
    stitcher_ = stitcher;

    stitcher_->subscribe([this](const StitchedFrame& frame) {
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

    encoder_running_ = true;
    composite_encoder_thread_ = std::thread(&PreviewService::compositeEncoderThreadFunc, this);
    composite_available_ = true;
}

void PreviewService::compositeEncoderThreadFunc() {
    std::vector<uint8_t> nv12_copy;

    // 尝试打开 jpeg_taco 硬件编码器，失败则退回 mjpeg 软编码
    const AVCodec* codec = avcodec_find_encoder_by_name("jpeg_taco");
    std::string codec_name = "jpeg_taco";
    if (!codec) {
        codec = avcodec_find_encoder_by_name("mjpeg");
        codec_name = "mjpeg";
    }
    if (!codec) {
        std::cerr << "[PreviewService] 找不到 JPEG 编码器" << std::endl;
        return;
    }

    AVCodecContext* enc_ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    bool encoder_ready = false;
    int prev_w = 0, prev_h = 0;

    auto cleanup = [&]() {
        if (pkt) { av_packet_free(&pkt); pkt = nullptr; }
        if (frame) { av_frame_free(&frame); frame = nullptr; }
        if (enc_ctx) { avcodec_free_context(&enc_ctx); enc_ctx = nullptr; }
        encoder_ready = false;
    };

    pkt = av_packet_alloc();

    while (encoder_running_ && !stop_requested_) {
        int w, h;
        {
            std::unique_lock<std::mutex> lock(composite_raw_mutex_);
            composite_raw_cv_.wait_for(lock, std::chrono::milliseconds(200), [this] {
                return composite_raw_ready_.load() || !encoder_running_ || stop_requested_.load();
            });
            if (!encoder_running_ || stop_requested_) break;
            if (!composite_raw_ready_) continue;
            composite_raw_ready_ = false;

            w = composite_width_;
            h = composite_height_;
            size_t needed = static_cast<size_t>(w) * h * 3 / 2;
            if (w <= 0 || h <= 0 || composite_raw_buf_.size() < needed) continue;
            nv12_copy.resize(needed);
            memcpy(nv12_copy.data(), composite_raw_buf_.data(), needed);
        }

        // 分辨率变化时重建编码器
        if (!encoder_ready || w != prev_w || h != prev_h) {
            cleanup();
            pkt = av_packet_alloc();

            enc_ctx = avcodec_alloc_context3(codec);
            if (!enc_ctx) continue;

            enc_ctx->width = w;
            enc_ctx->height = h;
            enc_ctx->pix_fmt = AV_PIX_FMT_NV12;
            enc_ctx->time_base = {1, 30};
            enc_ctx->framerate = {30, 1};

            // jpeg_taco 需要 quality 参数
            if (codec_name == "jpeg_taco") {
                av_opt_set_int(enc_ctx->priv_data, "quality", 60, 0);
            } else {
                enc_ctx->global_quality = 6 * FF_QP2LAMBDA;
                enc_ctx->flags |= AV_CODEC_FLAG_QSCALE;
            }

            if (avcodec_open2(enc_ctx, codec, nullptr) < 0) {
                std::cerr << "[PreviewService] 打开 " << codec_name << " 编码器失败" << std::endl;
                avcodec_free_context(&enc_ctx);
                enc_ctx = nullptr;
                continue;
            }

            frame = av_frame_alloc();
            frame->format = AV_PIX_FMT_NV12;
            frame->width = w;
            frame->height = h;

            prev_w = w;
            prev_h = h;
            encoder_ready = true;
            std::cout << "[PreviewService] composite 编码器 " << codec_name
                      << " 已就绪 " << w << "x" << h << std::endl;
        }

        // 填充 AVFrame 数据指针（NV12: Y + UV 交错）
        frame->data[0] = nv12_copy.data();
        frame->data[1] = nv12_copy.data() + static_cast<size_t>(w) * h;
        frame->linesize[0] = w;
        frame->linesize[1] = w;
        frame->pts++;

        int ret = avcodec_send_frame(enc_ctx, frame);
        if (ret < 0) continue;

        while (ret >= 0) {
            ret = avcodec_receive_packet(enc_ctx, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            if (pkt->data && pkt->size > 0) {
                onJpegFrame("__composite__", pkt->data, static_cast<size_t>(pkt->size));
            }
            av_packet_unref(pkt);
        }
    }

    cleanup();
}

void PreviewService::streamCompositeMjpeg(FrameCallback cb) {
    streamMjpeg("__composite__", cb);
}

std::vector<uint8_t> PreviewService::compositeSnapshot() {
    return snapshot("__composite__");
}

json PreviewService::getChannelFps() const {
    std::lock_guard<std::mutex> lock(mutex_);
    json result = json::object();
    for (auto& [worker_id, fb] : frame_buffers_) {
        if (worker_id == "__composite__") continue;
        std::lock_guard<std::mutex> fb_lock(fb->mutex);
        result[worker_id] = std::round(fb->current_fps * 10.0) / 10.0;
    }
    return result;
}

void PreviewService::resetComposite() {
    // 停止 encoder 线程
    encoder_running_ = false;
    composite_raw_cv_.notify_all();
    if (composite_encoder_thread_.joinable()) {
        composite_encoder_thread_.join();
    }

    // 释放旧 stitcher（让 weak_ptr 指向新实例）
    stitcher_.reset();
    composite_available_ = false;
    composite_connecting_ = false;
}

bool PreviewService::hasCompositePreview() {
    if (composite_available_.load()) {
        return true;
    }

    bool already_connecting = false;
    if (!composite_connecting_.compare_exchange_strong(already_connecting, true)) {
        return false;
    }

    std::thread([this]() {
        try {
            if (stop_requested_.load()) { composite_connecting_ = false; return; }
            auto stitcher = FrameStitcherService::getInstance();
            if (stitcher && !stop_requested_.load()) {
                connectStitcher(stitcher);
            }
        } catch (...) {}
        if (!composite_available_.load()) {
            composite_connecting_ = false;
        }
    }).detach();

    return false;
}

} // namespace webui
