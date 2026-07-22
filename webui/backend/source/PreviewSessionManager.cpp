#include "PreviewSessionManager.hpp"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <utility>

#ifndef PREVIEW_SESSION_NO_FFMPEG
#include "bufferpool/pool/builder/BufferPoolBuilderFactory.hpp"
#include "bufferpool/pool/base/BufferPool.hpp"
#include "productionline/line/VideoProductionLine.hpp"
#include "productionline/worker/base/ComponentTopology.hpp"
#include "productionline/worker/config/WorkerConfigs.hpp"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}
#endif

namespace webui {

// ---------------------------------------------------------------------------
// Opaque handle
// ---------------------------------------------------------------------------

struct PreviewSessionManager::EncoderHandle {
    bool is_null = true;
    PreviewSessionConfig cfg;
    std::string worker_id;

#ifndef PREVIEW_SESSION_NO_FFMPEG
    std::unique_ptr<IBufferPoolBuilder> input_pool_builder;
    std::shared_ptr<BufferPool> input_pool;
    uint64_t input_pool_id = 0;
    std::unique_ptr<::VideoProductionLine> encode_pipeline;
    uint64_t encode_pool_id = 0;
    std::atomic<bool> pipeline_ready{false};
    std::mutex pipeline_mu;
    int src_width = 0;
    int src_height = 0;
    int src_pix_fmt = -1;
#endif
};

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

struct PreviewSessionManager::Session {
    std::string session_id;
    std::string worker_id;
    PreviewSessionConfig cfg;
    std::string stream_url;
};

struct PreviewSessionManager::EncoderInstance {
    std::string worker_id;
    PreviewSessionConfig cfg;
    std::atomic<PreviewSessionState> state{PreviewSessionState::STOPPED};
    std::unordered_set<std::string> session_ids;
    std::shared_ptr<EncoderHandle> handle;

    // Latest-only input queue (depth 1).
    std::mutex frame_mu;
    AVFrame* latest_frame = nullptr;
    std::condition_variable frame_cv;
    std::atomic<bool> accepting_frames{false};

    // JPEG broadcast (§8.4) — shared_ptr snapshot, not a destructive deque.
    mutable std::mutex jpeg_mu;
    std::shared_ptr<const std::vector<uint8_t>> latest_jpeg;
    std::atomic<uint64_t> jpeg_sequence{0};
    mutable std::condition_variable jpeg_cv;

    std::atomic<bool> stop_requested{false};
    std::string last_error;

    std::thread encode_thread;
    std::thread reader_thread;
    std::atomic<bool> threads_started{false};
    /// Set when encode thread already joined reader + cleaned resources.
    std::atomic<bool> failed_finalized{false};
};

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PreviewSessionManager::PreviewSessionManager() {
    factory_ = [](const std::string&, const PreviewSessionConfig&) {
        return makeNullEncoder();
    };
    destroyer_ = [](std::shared_ptr<EncoderHandle>& h) {
        h.reset();
    };
}

PreviewSessionManager::~PreviewSessionManager() {
    shutdown();
}

void PreviewSessionManager::setEncoderFactory(EncoderFactory f, EncoderDestroyer d) {
    std::lock_guard<std::mutex> lock(mu_);
    if (f) {
        factory_ = std::move(f);
    }
    if (d) {
        destroyer_ = std::move(d);
    }
}

std::shared_ptr<PreviewSessionManager::EncoderHandle>
PreviewSessionManager::makeNullEncoder() {
    auto h = std::make_shared<EncoderHandle>();
    h->is_null = true;
    return h;
}

std::shared_ptr<PreviewSessionManager::EncoderHandle>
PreviewSessionManager::makeJpegTacoEncoder(const std::string& worker_id,
                                           const PreviewSessionConfig& cfg) {
#ifdef PREVIEW_SESSION_NO_FFMPEG
    (void)worker_id;
    (void)cfg;
    return nullptr;
#else
    // Early codec presence check (dimensions open lazily on first frame).
    // Full HW open happens in ensurePipelineOpen; exhaustion there → ERROR.
    if (cfg.encoder.empty()) {
        return nullptr;
    }
    auto h = std::make_shared<EncoderHandle>();
    h->is_null = false;
    h->cfg = cfg;
    h->worker_id = worker_id;
    return h;
#endif
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string PreviewSessionManager::makeSessionId(uint64_t id) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "preview-%06llu",
                  static_cast<unsigned long long>(id));
    return std::string(buf);
}

std::string PreviewSessionManager::stateToString(PreviewSessionState s) {
    switch (s) {
    case PreviewSessionState::STOPPED:  return "STOPPED";
    case PreviewSessionState::STARTING: return "STARTING";
    case PreviewSessionState::RUNNING:  return "RUNNING";
    case PreviewSessionState::STOPPING: return "STOPPING";
    case PreviewSessionState::ERROR:    return "ERROR";
    }
    return "UNKNOWN";
}

nlohmann::json PreviewSessionManager::sessionToJson(const Session& s,
                                                    PreviewSessionState enc_state,
                                                    const std::string& last_error) {
    // §6.1 create fields (fps/quality/…) plus §6.3 status fields.
    // FPS counters are placeholders until encoder metrics land.
    return nlohmann::json{
        {"session_id", s.session_id},
        {"worker_id", s.worker_id},
        {"state", stateToString(enc_state)},
        {"stream_url", s.stream_url},
        {"fps", s.cfg.fps},
        {"quality", s.cfg.quality},
        {"encoder", s.cfg.encoder},
        {"target_fps", s.cfg.fps},
        {"source_fps", 0.0},
        {"encoded_fps", 0.0},
        {"sent_fps", 0.0},
        {"dropped_input_frames", 0},
        {"encoded_frames", 0},
        {"last_error", last_error},
    };
}

PreviewSessionResult PreviewSessionManager::makeError(
    int http_status,
    const std::string& code,
    const std::string& message) const {
    PreviewSessionResult r;
    r.ok = false;
    r.http_status = http_status;
    r.error_code = code;
    r.error_message = message;
    r.state = PreviewSessionState::ERROR;
    return r;
}

bool PreviewSessionManager::isShareableState(PreviewSessionState s) {
    return s == PreviewSessionState::RUNNING ||
           s == PreviewSessionState::STARTING;
}

std::shared_ptr<PreviewSessionManager::EncoderInstance>
PreviewSessionManager::takeStaleEncoderLocked(const std::string& worker_id) {
    // Caller holds mu_.
    auto eit = encoders_by_worker_.find(worker_id);
    if (eit == encoders_by_worker_.end()) {
        return nullptr;
    }
    auto enc = eit->second;
    const auto st = enc->state.load();
    if (isShareableState(st)) {
        return nullptr;
    }
    for (const auto& sid : enc->session_ids) {
        sessions_by_id_.erase(sid);
    }
    enc->session_ids.clear();
    encoders_by_worker_.erase(eit);
    return enc;
}

void PreviewSessionManager::startEncoderThreads(
    const std::shared_ptr<EncoderInstance>& enc) {
    if (!enc || !enc->handle || enc->handle->is_null) {
        return;
    }
    if (enc->threads_started.exchange(true)) {
        return;
    }
#ifndef PREVIEW_SESSION_NO_FFMPEG
    enc->stop_requested.store(false);
    enc->failed_finalized.store(false);
    enc->encode_thread =
        std::thread(&PreviewSessionManager::encodeThreadMain, this, enc);
    enc->reader_thread =
        std::thread(&PreviewSessionManager::readerThreadMain, enc);
#else
    (void)enc;
#endif
}

void PreviewSessionManager::destroyEncoderInstance(
    std::shared_ptr<EncoderInstance> enc) {
    if (!enc) {
        return;
    }

    // Encode-thread finalizeFailedEncoder already cleaned this instance.
    if (enc->failed_finalized.load()) {
        if (enc->encode_thread.joinable()) {
            enc->encode_thread.join();
        }
        enc->state.store(PreviewSessionState::STOPPED);
        return;
    }

    // §7.2 stop order:
    // 1) STOPPING  2) stop accepting  3) wake waiters
    // 4) drop latest frame  5) stop pipeline  6) join threads
    // 7) release pools  8) destroy handle  9) STOPPED
    enc->state.store(PreviewSessionState::STOPPING);
    enc->accepting_frames.store(false);
    enc->stop_requested.store(true);
    enc->frame_cv.notify_all();
    enc->jpeg_cv.notify_all();

    {
        std::lock_guard<std::mutex> fl(enc->frame_mu);
#ifndef PREVIEW_SESSION_NO_FFMPEG
        if (enc->latest_frame) {
            av_frame_unref(enc->latest_frame);
            av_frame_free(&enc->latest_frame);
        }
#endif
        enc->latest_frame = nullptr;
    }

#ifndef PREVIEW_SESSION_NO_FFMPEG
    // Stop pipeline so reader acquireFilled can unwind.
    if (enc->handle && !enc->handle->is_null) {
        std::lock_guard<std::mutex> pl(enc->handle->pipeline_mu);
        if (enc->handle->encode_pipeline) {
            enc->handle->encode_pipeline->stop();
        }
    }
#endif

    if (enc->encode_thread.joinable()) {
        enc->encode_thread.join();
    }
    if (enc->reader_thread.joinable()) {
        enc->reader_thread.join();
    }
    enc->threads_started.store(false);

#ifndef PREVIEW_SESSION_NO_FFMPEG
    if (enc->handle && !enc->handle->is_null) {
        std::lock_guard<std::mutex> pl(enc->handle->pipeline_mu);
        if (enc->handle->input_pool) {
            for (Buffer* buf : enc->handle->input_pool->getAllManagedBuffers()) {
                AVFrame* f = buf->getAVFrame();
                if (f) {
                    av_frame_unref(f);
                }
            }
            enc->handle->input_pool.reset();
        }
        enc->handle->input_pool_builder.reset();
        enc->handle->encode_pipeline.reset();
        enc->handle->pipeline_ready.store(false);
    }
#endif

    std::shared_ptr<EncoderHandle> handle;
    {
        handle = std::move(enc->handle);
        enc->handle.reset();
    }

    if (destroyer_ && handle) {
        destroyer_(handle);
    } else {
        handle.reset();
    }

    enc->state.store(PreviewSessionState::STOPPED);
}

#ifndef PREVIEW_SESSION_NO_FFMPEG

void PreviewSessionManager::finalizeFailedEncoder(
    std::shared_ptr<EncoderInstance> enc) {
    // Called FROM encode thread after fatal open failure.
    if (!enc || enc->failed_finalized.exchange(true)) {
        return;
    }

    enc->state.store(PreviewSessionState::ERROR);
    enc->accepting_frames.store(false);
    enc->stop_requested.store(true);
    enc->frame_cv.notify_all();
    enc->jpeg_cv.notify_all();

    {
        std::lock_guard<std::mutex> fl(enc->frame_mu);
        if (enc->latest_frame) {
            av_frame_unref(enc->latest_frame);
            av_frame_free(&enc->latest_frame);
        }
        enc->latest_frame = nullptr;
    }

    if (enc->handle && !enc->handle->is_null) {
        std::lock_guard<std::mutex> pl(enc->handle->pipeline_mu);
        if (enc->handle->encode_pipeline) {
            enc->handle->encode_pipeline->stop();
        }
        if (enc->handle->input_pool) {
            for (Buffer* buf : enc->handle->input_pool->getAllManagedBuffers()) {
                AVFrame* f = buf->getAVFrame();
                if (f) {
                    av_frame_unref(f);
                }
            }
            enc->handle->input_pool.reset();
        }
        enc->handle->input_pool_builder.reset();
        enc->handle->encode_pipeline.reset();
        enc->handle->pipeline_ready.store(false);
    }

    // Join reader so it cannot spin; do not join encode (current thread).
    if (enc->reader_thread.joinable()) {
        enc->reader_thread.join();
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        auto eit = encoders_by_worker_.find(enc->worker_id);
        if (eit != encoders_by_worker_.end() && eit->second == enc) {
            for (const auto& sid : enc->session_ids) {
                sessions_by_id_.erase(sid);
            }
            enc->session_ids.clear();
            encoders_by_worker_.erase(eit);
        }
    }

    std::shared_ptr<EncoderHandle> handle;
    {
        handle = std::move(enc->handle);
        enc->handle.reset();
    }
    if (destroyer_ && handle) {
        destroyer_(handle);
    } else {
        handle.reset();
    }

    enc->threads_started.store(false);
    enc->state.store(PreviewSessionState::STOPPED);
    enc->jpeg_cv.notify_all();
}

bool PreviewSessionManager::ensurePipelineOpen(EncoderInstance& enc,
                                               AVFrame* first_frame) {
    if (!enc.handle || enc.handle->is_null || !first_frame) {
        return false;
    }
    if (enc.handle->pipeline_ready.load()) {
        return true;
    }

    std::lock_guard<std::mutex> pl(enc.handle->pipeline_mu);
    if (enc.handle->pipeline_ready.load()) {
        return true;
    }
    if (enc.stop_requested.load()) {
        return false;
    }

    const int src_width = first_frame->width;
    const int src_height = first_frame->height;
    const AVPixelFormat src_pix_fmt =
        static_cast<AVPixelFormat>(first_frame->format);
    if (src_width <= 0 || src_height <= 0 || !first_frame->data[0]) {
        enc.last_error = "invalid first frame";
        return false; // retryable — stay STARTING
    }

    std::cerr << "[PreviewSession] open encoder worker=" << enc.worker_id
              << " " << src_width << "x" << src_height
              << " pix_fmt=" << src_pix_fmt
              << " encoder=" << enc.cfg.encoder << std::endl;

    auto mark_fatal = [&](const std::string& err) {
        enc.last_error = err;
        enc.state.store(PreviewSessionState::ERROR);
        enc.accepting_frames.store(false);
        enc.stop_requested.store(true);
        enc.frame_cv.notify_all();
        enc.jpeg_cv.notify_all();
    };

    {
        size_t frame_size =
            static_cast<size_t>(src_width) * static_cast<size_t>(src_height) * 3 / 2;
        enc.handle->input_pool_builder = BufferPoolBuilderFactory::create(
            BufferPoolBuilderFactory::AllocatorType::AVFRAME);
        enc.handle->input_pool_id =
            enc.handle->input_pool_builder->allocatePoolWithBuffers(
                4, frame_size, "PreviewEncodeInput", "ENCODE_INPUT");
        if (enc.handle->input_pool_id == 0) {
            mark_fatal("ENCODER_RESOURCE_EXHAUSTED");
            return false;
        }
        enc.handle->input_pool =
            ComponentTopology::getInstance()
                .getPool(enc.handle->input_pool_id)
                .lock();
        if (!enc.handle->input_pool) {
            mark_fatal("ENCODER_RESOURCE_EXHAUSTED");
            enc.handle->input_pool_builder.reset();
            return false;
        }
    }

    std::vector<std::string> encoder_candidates = {enc.cfg.encoder};
    if (enc.cfg.encoder != "mjpeg") {
        encoder_candidates.push_back("mjpeg");
    }

    bool started = false;
    for (const auto& enc_name : encoder_candidates) {
        WorkerConfig enc_config;
        enc_config.global.worker_type = WorkerType::FFMPEG_ENCODE;
        enc_config.encoder.width = src_width;
        enc_config.encoder.height = src_height;
        enc_config.encoder.name = enc_name;
        enc_config.encoder.enable_hardware =
            (enc_name.find("taco") != std::string::npos);
        enc_config.encoder.input_pix_fmt = static_cast<int>(src_pix_fmt);
        enc_config.encoder.jpeg.quality = enc.cfg.quality;
        enc_config.encoder.framerate_num = enc.cfg.fps > 0 ? enc.cfg.fps : 15;
        enc_config.encoder.framerate_den = 1;
        enc_config.encoder.gop_size = 1;
        enc_config.encoder.max_b_frames = 0;
        enc_config.data_source.buffer_count = 4;
        enc_config.data_source.buffer_mode = true;

        enc.handle->encode_pipeline =
            std::make_unique<::VideoProductionLine>(false, 1, false);

        if (enc.handle->encode_pipeline->start(enc_config)) {
            auto worker = enc.handle->encode_pipeline->getWorker();
            if (worker) {
                worker->setSourceBufferPool(
                    ComponentTopology::getInstance().getPool(
                        enc.handle->input_pool_id));
            }
            enc.handle->encode_pool_id =
                enc.handle->encode_pipeline->getWorkingBufferPoolId();
            enc.handle->src_width = src_width;
            enc.handle->src_height = src_height;
            enc.handle->src_pix_fmt = static_cast<int>(src_pix_fmt);
            started = true;
            std::cerr << "[PreviewSession] encoder '" << enc_name
                      << "' started input_pool=" << enc.handle->input_pool_id
                      << " output_pool=" << enc.handle->encode_pool_id
                      << std::endl;
            break;
        }

        std::cerr << "[PreviewSession] encoder '" << enc_name
                  << "' start failed" << std::endl;
        enc.handle->encode_pipeline.reset();
    }

    if (!started) {
        enc.handle->input_pool.reset();
        enc.handle->input_pool_builder.reset();
        mark_fatal("ENCODER_RESOURCE_EXHAUSTED");
        return false;
    }

    enc.handle->pipeline_ready.store(true);
    // Async open succeeded — promote STARTING → RUNNING.
    enc.state.store(PreviewSessionState::RUNNING);
    return true;
}

void PreviewSessionManager::encodeThreadMain(
    std::shared_ptr<EncoderInstance> enc) {
    if (!enc) {
        return;
    }

    const int fps = enc->cfg.fps > 0 ? enc->cfg.fps : 15;
    const auto interval = std::chrono::milliseconds(1000 / fps);
    auto next_ok = std::chrono::steady_clock::now();

    while (!enc->stop_requested.load()) {
        AVFrame* frame = nullptr;
        {
            std::unique_lock<std::mutex> lock(enc->frame_mu);
            enc->frame_cv.wait_for(lock, std::chrono::milliseconds(200), [&] {
                return enc->latest_frame != nullptr ||
                       enc->stop_requested.load();
            });
            if (enc->stop_requested.load()) {
                break;
            }
            if (!enc->latest_frame) {
                continue;
            }
            frame = enc->latest_frame;
            enc->latest_frame = nullptr;
        }

        // Throttle BEFORE avcodec_send_frame / pool submit (spec §13.4).
        auto now = std::chrono::steady_clock::now();
        if (now < next_ok) {
            av_frame_unref(frame);
            av_frame_free(&frame);
            continue;
        }
        next_ok = now + interval;

        if (!ensurePipelineOpen(*enc, frame)) {
            av_frame_unref(frame);
            av_frame_free(&frame);
            if (enc->state.load() == PreviewSessionState::ERROR) {
                finalizeFailedEncoder(enc);
                return;
            }
            continue;
        }

        auto& handle = enc->handle;
        if (!handle || !handle->input_pool) {
            av_frame_unref(frame);
            av_frame_free(&frame);
            continue;
        }

        Buffer* dst_buf = handle->input_pool->acquireFree(false, 0);
        if (!dst_buf) {
            av_frame_unref(frame);
            av_frame_free(&frame);
            continue;
        }

        AVFrame* dst_frame = dst_buf->getAVFrame();
        if (dst_frame) {
            av_frame_unref(dst_frame);
            if (av_frame_ref(dst_frame, frame) < 0) {
                handle->input_pool->releaseFree(dst_buf);
                av_frame_unref(frame);
                av_frame_free(&frame);
                continue;
            }
        }
        av_frame_unref(frame);
        av_frame_free(&frame);

        handle->input_pool->submitFilled(dst_buf);
    }
}

void PreviewSessionManager::readerThreadMain(
    std::shared_ptr<EncoderInstance> enc) {
    if (!enc || !enc->handle) {
        return;
    }

    // Wait until pipeline opens, or stop / ERROR (do not spin forever).
    while (!enc->stop_requested.load() &&
           !enc->handle->pipeline_ready.load() &&
           enc->state.load() != PreviewSessionState::ERROR &&
           enc->state.load() != PreviewSessionState::STOPPING &&
           enc->state.load() != PreviewSessionState::STOPPED) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (enc->stop_requested.load() ||
        enc->state.load() == PreviewSessionState::ERROR ||
        enc->state.load() == PreviewSessionState::STOPPING ||
        enc->state.load() == PreviewSessionState::STOPPED ||
        !enc->handle || !enc->handle->pipeline_ready.load()) {
        return;
    }

    const uint64_t pool_id = enc->handle->encode_pool_id;
    auto pool = ComponentTopology::getInstance().getPool(pool_id).lock();
    if (!pool) {
        std::cerr << "[PreviewSession] reader: encode pool missing\n";
        return;
    }

    while (!enc->stop_requested.load() &&
           enc->state.load() != PreviewSessionState::ERROR) {
        Buffer* buf = pool->acquireFilled(true, 200);
        if (!buf) {
            continue;
        }

        AVPacket* pkt = buf->getAVPacket();
        if (pkt && pkt->data && pkt->size > 0) {
            auto jpeg = std::make_shared<std::vector<uint8_t>>(
                pkt->data, pkt->data + pkt->size);
            {
                std::lock_guard<std::mutex> jl(enc->jpeg_mu);
                enc->latest_jpeg = std::move(jpeg);
            }
            enc->jpeg_sequence.fetch_add(1, std::memory_order_relaxed);
            enc->jpeg_cv.notify_all();
        }

        pool->releaseFilled(buf);
    }
}

#endif // !PREVIEW_SESSION_NO_FFMPEG

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

PreviewSessionResult PreviewSessionManager::startSession(
    const std::string& worker_id,
    const PreviewSessionConfig& cfg) {
    if (shutting_down_.load()) {
        return makeError(503, "SHUTTING_DOWN", "PreviewSessionManager is shutting down");
    }
    if (worker_id.empty()) {
        return makeError(400, "INVALID_WORKER", "worker_id is required");
    }
    if (cfg.fps < 1 || cfg.fps > 60) {
        return makeError(400, "INVALID_FPS", "fps must be in range 1..60");
    }
    if (cfg.quality < 1 || cfg.quality > 100) {
        return makeError(400, "INVALID_QUALITY", "quality must be in range 1..100");
    }

    std::vector<std::shared_ptr<EncoderInstance>> to_destroy;
    EncoderFactory factory;
    PreviewSessionResult early;
    bool have_early = false;

    {
        std::lock_guard<std::mutex> lock(mu_);
        factory = factory_;

        // Never re-share ERROR/STOPPING/STOPPED — detach for teardown.
        if (auto stale = takeStaleEncoderLocked(worker_id)) {
            to_destroy.push_back(std::move(stale));
        }

        auto enc_it = encoders_by_worker_.find(worker_id);
        if (enc_it != encoders_by_worker_.end()) {
            auto& enc = enc_it->second;
            const auto st = enc->state.load();
            if (!isShareableState(st)) {
                if (auto stale = takeStaleEncoderLocked(worker_id)) {
                    to_destroy.push_back(std::move(stale));
                }
            } else if (!(enc->cfg == cfg)) {
                early = makeError(409, "CONFIG_CONFLICT",
                    "Active encoder config differs for this worker");
                have_early = true;
            } else {
                const uint64_t id = next_id_.fetch_add(1);
                auto session = std::make_shared<Session>();
                session->session_id = makeSessionId(id);
                session->worker_id = worker_id;
                session->cfg = cfg;
                session->stream_url =
                    "/api/preview/stream/" + worker_id +
                    "?session_id=" + session->session_id;

                enc->session_ids.insert(session->session_id);
                sessions_by_id_[session->session_id] = session;

                early.ok = true;
                early.http_status = 200;
                early.session_id = session->session_id;
                early.worker_id = worker_id;
                early.stream_url = session->stream_url;
                early.state = st;
                have_early = true;
            }
        }
    }

    for (auto& enc : to_destroy) {
        destroyEncoderInstance(std::move(enc));
    }
    to_destroy.clear();

    if (have_early) {
        return early;
    }

    auto handle = factory ? factory(worker_id, cfg) : nullptr;
    if (!handle) {
        return makeError(503, "ENCODER_RESOURCE_EXHAUSTED",
                         "Failed to create preview encoder");
    }

    std::shared_ptr<EncoderInstance> created;
    PreviewSessionResult result;
    bool result_set = false;

    {
        std::lock_guard<std::mutex> lock(mu_);
        if (shutting_down_.load()) {
            auto orphan = std::make_shared<EncoderInstance>();
            orphan->handle = std::move(handle);
            to_destroy.push_back(std::move(orphan));
            result = makeError(503, "SHUTTING_DOWN",
                               "PreviewSessionManager is shutting down");
            result_set = true;
        } else {
            if (auto stale = takeStaleEncoderLocked(worker_id)) {
                to_destroy.push_back(std::move(stale));
            }

            auto enc_it = encoders_by_worker_.find(worker_id);
            if (enc_it != encoders_by_worker_.end()) {
                auto& enc = enc_it->second;
                const auto st = enc->state.load();
                if (isShareableState(st) && (enc->cfg == cfg)) {
                    auto orphan = std::make_shared<EncoderInstance>();
                    orphan->handle = std::move(handle);
                    to_destroy.push_back(std::move(orphan));

                    const uint64_t id = next_id_.fetch_add(1);
                    auto session = std::make_shared<Session>();
                    session->session_id = makeSessionId(id);
                    session->worker_id = worker_id;
                    session->cfg = cfg;
                    session->stream_url =
                        "/api/preview/stream/" + worker_id +
                        "?session_id=" + session->session_id;
                    enc->session_ids.insert(session->session_id);
                    sessions_by_id_[session->session_id] = session;

                    result.ok = true;
                    result.http_status = 200;
                    result.session_id = session->session_id;
                    result.worker_id = worker_id;
                    result.stream_url = session->stream_url;
                    result.state = st;
                    result_set = true;
                } else if (isShareableState(st) && !(enc->cfg == cfg)) {
                    auto orphan = std::make_shared<EncoderInstance>();
                    orphan->handle = std::move(handle);
                    to_destroy.push_back(std::move(orphan));
                    result = makeError(409, "CONFIG_CONFLICT",
                        "Active encoder config differs for this worker");
                    result_set = true;
                } else {
                    if (auto stale = takeStaleEncoderLocked(worker_id)) {
                        to_destroy.push_back(std::move(stale));
                    }
                }
            }

            if (!result_set &&
                encoders_by_worker_.find(worker_id) == encoders_by_worker_.end()) {
                auto enc = std::make_shared<EncoderInstance>();
                enc->worker_id = worker_id;
                enc->cfg = cfg;
                enc->handle = std::move(handle);

                const uint64_t id = next_id_.fetch_add(1);
                auto session = std::make_shared<Session>();
                session->session_id = makeSessionId(id);
                session->worker_id = worker_id;
                session->cfg = cfg;
                session->stream_url =
                    "/api/preview/stream/" + worker_id +
                    "?session_id=" + session->session_id;

                enc->session_ids.insert(session->session_id);
                enc->accepting_frames.store(true);

                // Null encoder: no HW open → RUNNING.
                // Real jpeg_taco: STARTING until ensurePipelineOpen succeeds.
                if (enc->handle && enc->handle->is_null) {
                    enc->state.store(PreviewSessionState::RUNNING);
                } else {
                    enc->state.store(PreviewSessionState::STARTING);
                }

                encoders_by_worker_[worker_id] = enc;
                sessions_by_id_[session->session_id] = session;
                created = enc;

                result.ok = true;
                result.http_status = 200;
                result.session_id = session->session_id;
                result.worker_id = worker_id;
                result.stream_url = session->stream_url;
                result.state = enc->state.load();
                result_set = true;
            } else if (!result_set && handle) {
                auto orphan = std::make_shared<EncoderInstance>();
                orphan->handle = std::move(handle);
                to_destroy.push_back(std::move(orphan));
                result = makeError(503, "ENCODER_RESOURCE_EXHAUSTED",
                                   "Failed to install preview encoder");
                result_set = true;
            }
        }
    }

    for (auto& enc : to_destroy) {
        destroyEncoderInstance(std::move(enc));
    }
    if (created) {
        startEncoderThreads(created);
    }
    if (!result_set) {
        return makeError(503, "ENCODER_RESOURCE_EXHAUSTED",
                         "Failed to install preview encoder");
    }
    return result;
}

PreviewSessionResult PreviewSessionManager::stopSession(
    const std::string& session_id) {
    std::shared_ptr<EncoderInstance> to_destroy;

    {
        std::lock_guard<std::mutex> lock(mu_);
        auto sit = sessions_by_id_.find(session_id);
        if (sit == sessions_by_id_.end()) {
            PreviewSessionResult r;
            r.ok = true;
            r.http_status = 200;
            r.session_id = session_id;
            r.state = PreviewSessionState::STOPPED;
            return r;
        }

        const std::string worker_id = sit->second->worker_id;
        sessions_by_id_.erase(sit);

        auto eit = encoders_by_worker_.find(worker_id);
        if (eit != encoders_by_worker_.end()) {
            auto& enc = eit->second;
            enc->session_ids.erase(session_id);
            if (enc->session_ids.empty()) {
                to_destroy = enc;
                encoders_by_worker_.erase(eit);
                to_destroy->state.store(PreviewSessionState::STOPPING);
                to_destroy->accepting_frames.store(false);
            }
        }
    }

    if (to_destroy) {
        destroyEncoderInstance(std::move(to_destroy));
    }

    PreviewSessionResult r;
    r.ok = true;
    r.http_status = 200;
    r.session_id = session_id;
    r.state = PreviewSessionState::STOPPED;
    return r;
}

void PreviewSessionManager::stopAllForWorker(const std::string& worker_id) {
    std::vector<std::string> session_ids;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto eit = encoders_by_worker_.find(worker_id);
        if (eit == encoders_by_worker_.end()) {
            return;
        }
        session_ids.assign(eit->second->session_ids.begin(),
                           eit->second->session_ids.end());
    }
    for (const auto& sid : session_ids) {
        stopSession(sid);
    }
}

void PreviewSessionManager::shutdown() {
    shutting_down_.store(true);

    std::vector<std::string> session_ids;
    {
        std::lock_guard<std::mutex> lock(mu_);
        session_ids.reserve(sessions_by_id_.size());
        for (const auto& kv : sessions_by_id_) {
            session_ids.push_back(kv.first);
        }
    }
    for (const auto& sid : session_ids) {
        stopSession(sid);
    }
}

bool PreviewSessionManager::hasActiveSession(const std::string& worker_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = encoders_by_worker_.find(worker_id);
    if (it == encoders_by_worker_.end()) {
        return false;
    }
    const auto st = it->second->state.load();
    return st == PreviewSessionState::RUNNING ||
           st == PreviewSessionState::STARTING;
}

bool PreviewSessionManager::offerFrame(const std::string& worker_id,
                                       AVFrame* frame) {
    if (!frame) {
        return false;
    }

    std::shared_ptr<EncoderInstance> enc;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = encoders_by_worker_.find(worker_id);
        if (it == encoders_by_worker_.end()) {
            return false;
        }
        enc = it->second;
    }

    {
        const auto st = enc->state.load();
        // Accept during STARTING (feed first frame for async open) and RUNNING.
        if (!enc->accepting_frames.load() || !isShareableState(st)) {
            return false;
        }
    }
    if (!enc->handle) {
        return false;
    }

#ifdef PREVIEW_SESSION_NO_FFMPEG
    // Host unit-test build: accept without retaining (no libav link).
    (void)frame;
    return true;
#else
    if (enc->handle->is_null) {
        return true;
    }

    AVFrame* ref = av_frame_alloc();
    if (!ref) {
        return false;
    }
    if (av_frame_ref(ref, frame) < 0) {
        av_frame_free(&ref);
        return false;
    }

    {
        std::lock_guard<std::mutex> fl(enc->frame_mu);
        if (!enc->accepting_frames.load()) {
            av_frame_unref(ref);
            av_frame_free(&ref);
            return false;
        }
        if (enc->latest_frame) {
            av_frame_unref(enc->latest_frame);
            av_frame_free(&enc->latest_frame);
        }
        enc->latest_frame = ref;
    }
    enc->frame_cv.notify_one();
    return true;
#endif
}

bool PreviewSessionManager::waitLatestJpeg(const std::string& worker_id,
                                           uint64_t after_seq,
                                           PreviewJpegSnapshot& out,
                                           int wait_ms) const {
    std::shared_ptr<EncoderInstance> enc;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = encoders_by_worker_.find(worker_id);
        if (it == encoders_by_worker_.end()) {
            return false;
        }
        enc = it->second;
    }

    std::unique_lock<std::mutex> jl(enc->jpeg_mu);
    enc->jpeg_cv.wait_for(jl, std::chrono::milliseconds(wait_ms), [&] {
        const auto st = enc->state.load();
        return enc->jpeg_sequence.load() > after_seq ||
               enc->stop_requested.load() ||
               st == PreviewSessionState::STOPPING ||
               st == PreviewSessionState::STOPPED ||
               st == PreviewSessionState::ERROR;
    });

    const uint64_t seq = enc->jpeg_sequence.load();
    if (seq <= after_seq || !enc->latest_jpeg) {
        return false;
    }
    out.data = enc->latest_jpeg; // shared_ptr copy — never clear shared queue
    out.sequence = seq;
    return true;
}

int PreviewSessionManager::activeEncoderCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return static_cast<int>(encoders_by_worker_.size());
}

nlohmann::json PreviewSessionManager::listSessions() const {
    std::lock_guard<std::mutex> lock(mu_);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& kv : sessions_by_id_) {
        const auto& s = *kv.second;
        PreviewSessionState st = PreviewSessionState::STOPPED;
        std::string last_error;
        auto eit = encoders_by_worker_.find(s.worker_id);
        if (eit != encoders_by_worker_.end()) {
            st = eit->second->state.load();
            last_error = eit->second->last_error;
        }
        arr.push_back(sessionToJson(s, st, last_error));
    }
    return arr;
}

nlohmann::json PreviewSessionManager::getSession(
    const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto sit = sessions_by_id_.find(session_id);
    if (sit == sessions_by_id_.end()) {
        return nullptr;
    }
    const auto& s = *sit->second;
    PreviewSessionState st = PreviewSessionState::STOPPED;
    std::string last_error;
    auto eit = encoders_by_worker_.find(s.worker_id);
    if (eit != encoders_by_worker_.end()) {
        st = eit->second->state.load();
        last_error = eit->second->last_error;
    }
    return sessionToJson(s, st, last_error);
}

bool PreviewSessionManager::testingMarkEncoderError(const std::string& worker_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = encoders_by_worker_.find(worker_id);
    if (it == encoders_by_worker_.end()) {
        return false;
    }
    it->second->state.store(PreviewSessionState::ERROR);
    it->second->accepting_frames.store(false);
    it->second->stop_requested.store(true);
    it->second->frame_cv.notify_all();
    it->second->jpeg_cv.notify_all();
    return true;
}

} // namespace webui
