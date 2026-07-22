#pragma once

/**
 * @file PreviewFrameTap.hpp
 * @brief Light decode-path tap for on-demand WebUI preview sessions.
 *
 * Idle path (no active session): hasActiveSession check only — no alloc,
 * memcpy, or encoder open. Active path: offerFrame (av_frame_ref inside).
 *
 * consume() always returns true so MultiConsumer continues other strategies.
 * shouldRetainBuffer() stays false (default) — FrameTap never retains the
 * decode Buffer; ownership is via av_frame_ref into the encoder queue.
 */

#include "consumptionline/core/IBufferConsumer.hpp"

#include <cstdint>
#include <string>

namespace webui {

class PreviewSessionManager;

class PreviewFrameTapConsumer : public consumer::IBufferConsumer {
public:
    PreviewFrameTapConsumer(PreviewSessionManager* mgr, std::string worker_id);

    bool initialize(const std::vector<Buffer*>& first_buffers) override;
    bool consume(const std::vector<Buffer*>& buffers, int frame_index) override;
    std::string getStats() const override;

private:
    PreviewSessionManager* mgr_ = nullptr;
    std::string worker_id_;
    uint64_t offered_ = 0;
    uint64_t idle_hits_ = 0;
};

} // namespace webui
