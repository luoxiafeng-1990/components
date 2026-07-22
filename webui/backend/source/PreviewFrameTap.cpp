#include "PreviewFrameTap.hpp"
#include "PreviewSessionManager.hpp"

namespace webui {

PreviewFrameTapConsumer::PreviewFrameTapConsumer(PreviewSessionManager* mgr,
                                                 std::string worker_id)
    : mgr_(mgr)
    , worker_id_(std::move(worker_id))
{
}

bool PreviewFrameTapConsumer::initialize(const std::vector<Buffer*>& /*first_buffers*/) {
    // No encoder / pool setup — sessions own the encoder lifecycle.
    return true;
}

bool PreviewFrameTapConsumer::consume(const std::vector<Buffer*>& buffers,
                                      int /*frame_index*/) {
    // Idle fast path (spec §8.2): no alloc, memcpy, or encoder open.
    if (!mgr_ || !mgr_->hasActiveSession(worker_id_)) {
        ++idle_hits_;
        return true; // continue MultiConsumer; do not retain buffer
    }

    if (buffers.empty() || !buffers[0]) {
        return true;
    }

    AVFrame* f = buffers[0]->getAVFrame();
    if (!f) {
        return true;
    }

    if (mgr_->offerFrame(worker_id_, f)) {
        ++offered_;
    }
    return true;
}

std::string PreviewFrameTapConsumer::getStats() const {
    return "PreviewFrameTap: offered=" + std::to_string(offered_) +
           " idle_hits=" + std::to_string(idle_hits_);
}

} // namespace webui
