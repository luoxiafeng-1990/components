#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

struct AVFrame;

namespace webui {

struct PreviewSessionConfig {
    int fps = 15;
    int quality = 80;
    std::string encoder = "jpeg_taco";
    bool operator==(const PreviewSessionConfig& o) const {
        return fps == o.fps && quality == o.quality && encoder == o.encoder;
    }
};

enum class PreviewSessionState { STOPPED, STARTING, RUNNING, STOPPING, ERROR };

struct PreviewSessionResult {
    bool ok = false;
    int http_status = 200;
    std::string error_code;   // e.g. ENCODER_RESOURCE_EXHAUSTED
    std::string error_message;
    std::string session_id;
    std::string worker_id;
    std::string stream_url;
    PreviewSessionState state = PreviewSessionState::STOPPED;
};

/// Non-destructive JPEG snapshot for MJPEG clients (spec §8.4).
struct PreviewJpegSnapshot {
    std::shared_ptr<const std::vector<uint8_t>> data;
    uint64_t sequence = 0;
};

class PreviewSessionManager {
public:
    struct EncoderHandle; // opaque; real jpeg_taco resources inside

    using EncoderFactory = std::function<std::shared_ptr<EncoderHandle>(
        const std::string& worker_id, const PreviewSessionConfig& cfg)>;
    using EncoderDestroyer = std::function<void(std::shared_ptr<EncoderHandle>&)>;

    PreviewSessionManager();
    ~PreviewSessionManager();

    PreviewSessionManager(const PreviewSessionManager&) = delete;
    PreviewSessionManager& operator=(const PreviewSessionManager&) = delete;

    void setEncoderFactory(EncoderFactory f, EncoderDestroyer d);
    static std::shared_ptr<EncoderHandle> makeNullEncoder();

    /// Board / webui_server factory: BufferPool + jpeg_taco VideoProductionLine.
    /// Returns null → START fails with ENCODER_RESOURCE_EXHAUSTED.
    /// Host unit tests use makeNullEncoder (PREVIEW_SESSION_NO_FFMPEG).
    static std::shared_ptr<EncoderHandle> makeJpegTacoEncoder(
        const std::string& worker_id, const PreviewSessionConfig& cfg);

    PreviewSessionResult startSession(const std::string& worker_id,
                                      const PreviewSessionConfig& cfg);
    PreviewSessionResult stopSession(const std::string& session_id);
    void stopAllForWorker(const std::string& worker_id);
    void shutdown();

    bool hasActiveSession(const std::string& worker_id) const;
    bool offerFrame(const std::string& worker_id, AVFrame* frame); // av_frame_ref

    /// Wait for a JPEG with sequence > after_seq. Does not clear shared state.
    bool waitLatestJpeg(const std::string& worker_id,
                        uint64_t after_seq,
                        PreviewJpegSnapshot& out,
                        int wait_ms) const;

    int activeEncoderCount() const;
    nlohmann::json listSessions() const;
    nlohmann::json getSession(const std::string& session_id) const;

    /// Test-only: force encoder into ERROR without teardown (share-path rejection).
    bool testingMarkEncoderError(const std::string& worker_id);

private:
    struct Session;
    struct EncoderInstance;
    // map lock only for maps; join encoder outside lock (spec §16)
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_by_id_;
    std::unordered_map<std::string, std::shared_ptr<EncoderInstance>> encoders_by_worker_;
    std::atomic<uint64_t> next_id_{1};
    EncoderFactory factory_;
    EncoderDestroyer destroyer_;
    std::atomic<bool> shutting_down_{false};

    static std::string makeSessionId(uint64_t id);
    static std::string stateToString(PreviewSessionState s);
    static nlohmann::json sessionToJson(const Session& s,
                                        PreviewSessionState enc_state,
                                        const std::string& last_error = "");

    PreviewSessionResult makeError(int http_status,
                                   const std::string& code,
                                   const std::string& message) const;

    /// True if encoder may accept new shared sessions (not ERROR/STOPPING/STOPPED).
    static bool isShareableState(PreviewSessionState s);

    /// Detach a non-shareable encoder from maps; caller destroys outside lock.
    std::shared_ptr<EncoderInstance> takeStaleEncoderLocked(
        const std::string& worker_id);

    void startEncoderThreads(const std::shared_ptr<EncoderInstance>& enc);
    void destroyEncoderInstance(std::shared_ptr<EncoderInstance> enc);

#ifndef PREVIEW_SESSION_NO_FFMPEG
    /// Called from encode thread on fatal open failure: stop reader, erase maps.
    void finalizeFailedEncoder(std::shared_ptr<EncoderInstance> enc);

    void encodeThreadMain(std::shared_ptr<EncoderInstance> enc);
    static void readerThreadMain(std::shared_ptr<EncoderInstance> enc);
    static bool ensurePipelineOpen(EncoderInstance& enc, AVFrame* first_frame);
#endif
};

} // namespace webui
