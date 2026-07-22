#include "consumptionline/types/stitcher/FrameStitcherService.hpp"
#include "common/ImageMeta.hpp"

#include <cmath>
#include <sstream>
#include <algorithm>

// === Define Static Instance Members ===
std::mutex FrameStitcherService::s_instance_mutex;
std::weak_ptr<FrameStitcherService> FrameStitcherService::s_active_instance;

std::shared_ptr<FrameStitcherService> FrameStitcherService::getInstance() {
    std::lock_guard<std::mutex> lock(s_instance_mutex);
    return s_active_instance.lock();
}

// ============================================================
// Anonymous-namespace helpers (extracted from TacoProDisplayContext.cpp)
// ============================================================

namespace {

int selectGridCount(int max_channels) {
    constexpr int presets[] = {1, 2, 4, 9, 16, 25, 36};
    for (int g : presets) {
        if (max_channels <= g) return g;
    }
    int n = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(max_channels))));
    return n * n;
}

void computeGridSlots(int count, int screen_w, int screen_h,
                      std::vector<ChannelLayout>& slots) {
    int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
    int rows = (count + cols - 1) / cols;
    int cell_w = screen_w / cols;
    int cell_h = screen_h / rows;

    slots.resize(count);
    for (int i = 0; i < count; ++i) {
        slots[i] = {(i % cols) * cell_w, (i / cols) * cell_h, cell_w, cell_h};
    }
}

void computeMainSidebarSlots(int screen_w, int screen_h, float ratio,
                              std::vector<ChannelLayout>& slots) {
    int main_w = static_cast<int>(screen_w * ratio);
    int side_w = screen_w - main_w;
    int side_h = screen_h / 4;

    slots.resize(5);
    slots[0] = {0, 0, main_w, screen_h};
    for (int i = 0; i < 4; ++i) {
        slots[1 + i] = {main_w, i * side_h, side_w, side_h};
    }
}

} // anonymous namespace

// ============================================================
// Constructor / Destructor
// ============================================================

FrameStitcherService::FrameStitcherService(
    const FrameStitcherConfig& config,
    std::shared_ptr<IFrameStitcherDriver> driver,
    std::shared_ptr<BufferPool> pool,
    std::unique_ptr<Buffer> template_buf)
    : screen_width_(config.screen_width)
    , screen_height_(config.screen_height)
    , bits_per_pixel_(config.bits_per_pixel)
    , buffer_size_(0)
    , config_(config)
    , driver_(std::move(driver))
    , pool_(std::move(pool))
    , template_buf_(std::move(template_buf))
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.FrameStitcherService")))
{
    size_t total_bits = static_cast<size_t>(screen_width_) * screen_height_ * bits_per_pixel_;
    buffer_size_ = (total_bits + 7) / 8;

    channels_.reserve(64);
    createView();
}

FrameStitcherService::~FrameStitcherService() {
    stop();
}

// ============================================================
// Lifecycle
// ============================================================

bool FrameStitcherService::start() {
    if (running_) return true;

    int fps = config_.target_fps > 0 ? config_.target_fps : 30;
    int display_interval_ms = 1000 / fps;
    frame_timeout_ms_ = display_interval_ms * 2;

    running_ = true;
    render_thread_ = std::thread(&FrameStitcherService::renderThreadFunc, this);

    // Start the display tick timer (consumes FILLED, notifies subscribers)
    timer_.start();
    timer_id_ = timer_.scheduleRepeated(display_interval_ms,
                                        [this]() { onTick(); });

    LOG4CPLUS_INFO_FMT(logger_,
        "FrameStitcherService started (fps=%d, tick_interval=%dms, frame_timeout=%dms)",
        fps, display_interval_ms, frame_timeout_ms_);

    {
        std::lock_guard<std::mutex> lock(s_instance_mutex);
        s_active_instance = shared_from_this();
    }

    return true;
}

void FrameStitcherService::stop() {
    if (!running_) return;

    {
        std::lock_guard<std::mutex> lock(s_instance_mutex);
        if (s_active_instance.lock() == shared_from_this()) {
            s_active_instance.reset();
        }
    }

    // Stop tick timer first
    if (timer_id_ != 0) {
        timer_.cancel(timer_id_);
        timer_id_ = 0;
    }
    timer_.stop();

    // Stop render thread
    running_ = false;
    round_cv_.notify_all();
    render_cv_.notify_all();

    if (render_thread_.joinable()) {
        render_thread_.join();
    }

    // Release displayed buffer
    if (displayed_buf_) {
        pool_->releaseFilled(displayed_buf_);
        displayed_buf_ = nullptr;
    }

    LOG4CPLUS_INFO(logger_, "FrameStitcherService stopped");
}

// ============================================================
// View management
// ============================================================

void FrameStitcherService::createView() {
    if (config_.view_type == "main_sidebar") {
        view_type_ = ViewType::MAIN_SIDEBAR;
        computeMainSidebarSlots(screen_width_, screen_height_,
                                config_.main_sidebar_ratio, view_slots_);
    } else {
        view_type_ = ViewType::GRID;
        int grid_count = selectGridCount(1);
        computeGridSlots(grid_count, screen_width_, screen_height_, view_slots_);
    }

    slot_assignment_ = config_.slot_assignment;

    LOG4CPLUS_INFO_FMT(logger_, "View created: type=%s, slots=%d, assignment_size=%d",
        (view_type_ == ViewType::GRID ? "grid" : "main_sidebar"),
        static_cast<int>(view_slots_.size()),
        static_cast<int>(slot_assignment_.size()));
}

const ChannelLayout&
FrameStitcherService::resolveLayout(int channel_id) const {
    if (!slot_assignment_.empty()) {
        for (int i = 0; i < static_cast<int>(slot_assignment_.size()); ++i) {
            if (slot_assignment_[i] == channel_id) {
                return view_slots_.at(i);
            }
        }
    }
    return view_slots_.at(channel_id);
}

const ChannelLayout&
FrameStitcherService::getSlotLayout(int slot_index) const {
    return view_slots_.at(slot_index);
}

// ============================================================
// Channel management
// ============================================================

int FrameStitcherService::registerChannel() {
    std::lock_guard<std::mutex> lock(channel_mgmt_mutex_);

    int id = next_channel_id_++;

    if (view_type_ == ViewType::GRID &&
        id >= static_cast<int>(view_slots_.size())) {
        int new_grid = selectGridCount(id + 1);
        computeGridSlots(new_grid, screen_width_, screen_height_, view_slots_);

        for (auto& existing : channels_) {
            existing.layout = view_slots_.at(existing.channel_id);
        }

        LOG4CPLUS_INFO_FMT(logger_,
            "Grid expanded to %d slots for channel %d", new_grid, id);
    }

    if (id >= static_cast<int>(view_slots_.size())) {
        LOG4CPLUS_ERROR_FMT(logger_,
            "Channel %d exceeds view slot count (%d)", id,
            static_cast<int>(view_slots_.size()));
        --next_channel_id_;
        return -1;
    }

    const auto& layout = resolveLayout(id);

    ChannelInfo ch;
    ch.channel_id = id;
    ch.layout = layout;
    ch.active = true;
    channels_.push_back(ch);

    LOG4CPLUS_INFO_FMT(logger_,
        "Channel %d registered: slot region=(%d,%d,%d,%d)",
        id, layout.x, layout.y, layout.w, layout.h);

    return id;
}

int FrameStitcherService::registerChannel(const ChannelLayout& layout) {
    std::lock_guard<std::mutex> lock(channel_mgmt_mutex_);

    int id = next_channel_id_++;
    ChannelInfo ch;
    ch.channel_id = id;
    ch.layout = layout;
    ch.active = true;
    channels_.push_back(ch);

    LOG4CPLUS_INFO_FMT(logger_,
        "Channel %d registered (manual): region=(%d,%d,%d,%d)",
        id, layout.x, layout.y, layout.w, layout.h);

    return id;
}

void FrameStitcherService::unregisterChannel(int channel_id) {
    {
        std::lock_guard<std::mutex> lock(channel_mgmt_mutex_);
        for (auto& ch : channels_) {
            if (ch.channel_id == channel_id) {
                ch.active = false;
                ch.worker_id.clear();
                LOG4CPLUS_INFO_FMT(logger_, "Channel %d unregistered", channel_id);
                break;
            }
        }
    }
    // Wake any threads blocked in channelWrite for this channel
    round_cv_.notify_all();
}

void FrameStitcherService::setChannelWorkerId(int channel_id,
                                              const std::string& worker_id) {
    std::lock_guard<std::mutex> lock(channel_mgmt_mutex_);
    for (auto& ch : channels_) {
        if (ch.channel_id == channel_id) {
            ch.worker_id = worker_id;
            LOG4CPLUS_INFO_FMT(logger_,
                "Channel %d worker_id set to '%s'",
                channel_id, worker_id.c_str());
            return;
        }
    }
    LOG4CPLUS_WARN_FMT(logger_,
        "setChannelWorkerId: channel %d not found", channel_id);
}

LayoutSnapshot FrameStitcherService::getLayoutSnapshot() {
    std::lock_guard<std::mutex> lock(channel_mgmt_mutex_);

    LayoutSnapshot snap;
    snap.width = screen_width_;
    snap.height = screen_height_;
    snap.view_type = (view_type_ == ViewType::MAIN_SIDEBAR) ? "main_sidebar" : "grid";

    const int slot_count = static_cast<int>(view_slots_.size());
    if (view_type_ == ViewType::MAIN_SIDEBAR) {
        snap.cols = 2;
        snap.rows = 4;
    } else if (slot_count > 0) {
        snap.cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(slot_count))));
        snap.rows = (slot_count + snap.cols - 1) / snap.cols;
    }

    snap.slots.reserve(static_cast<size_t>(slot_count));
    for (int i = 0; i < slot_count; ++i) {
        LayoutSlotSnapshot slot;
        slot.slot = i;
        slot.x = view_slots_[i].x;
        slot.y = view_slots_[i].y;
        slot.width = view_slots_[i].w;
        slot.height = view_slots_[i].h;

        int channel_id = -1;
        if (!slot_assignment_.empty()) {
            if (i < static_cast<int>(slot_assignment_.size())) {
                channel_id = slot_assignment_[i];
            }
        } else {
            // Default mapping: channel_id == slot index when that channel is active
            for (const auto& ch : channels_) {
                if (ch.active && ch.channel_id == i) {
                    channel_id = i;
                    break;
                }
            }
        }

        slot.channel_id = channel_id;
        if (channel_id >= 0) {
            for (const auto& ch : channels_) {
                if (ch.channel_id == channel_id) {
                    slot.worker_id = ch.worker_id;
                    break;
                }
            }
        }

        snap.slots.push_back(std::move(slot));
    }

    return snap;
}

// ============================================================
// Channel write (wait for render thread to open a new round)
// ============================================================

bool FrameStitcherService::channelWrite(int channel_id, Buffer* decoded) {
    if (!decoded) return false;

    ChannelInfo* ch_info = nullptr;
    {
        std::lock_guard<std::mutex> mgmt_lock(channel_mgmt_mutex_);
        for (auto& ch : channels_) {
            if (ch.channel_id == channel_id && ch.active) {
                ch_info = &ch;
                break;
            }
        }
    }

    if (!ch_info) {
        LOG4CPLUS_WARN_FMT(logger_, "Channel %d not found or inactive", channel_id);
        return false;
    }

    // Wait for the render thread to start a new round, then write
    uint64_t my_round = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> round_lock(round_mutex_);
            round_cv_.wait(round_lock, [&]() {
                return (round_seq_ > my_round && !ch_info->written_this_round)
                       || !running_ || !ch_info->active;
            });
            if (!running_ || !ch_info->active) return false;
            my_round = round_seq_;
        }

        {
            std::shared_lock<std::shared_mutex> lock(rw_mutex_);

            if (render_buf_ == nullptr) {
                // Round already ended, wait for next one
                continue;
            }

            auto dec_img = ImageMeta::fromBuffer(decoded);
            int src_width  = dec_img.width();
            int src_height = dec_img.height();

            if (src_width <= 0 || src_height <= 0 || !decoded->getAVFrame()) {
                LOG4CPLUS_WARN_FMT(logger_,
                    "Channel %d: invalid decoded frame (%dx%d, avframe=%p)",
                    channel_id, src_width, src_height, (void*)decoded->getAVFrame());
                return false;
            }

            AVFrame* avf = decoded->getAVFrame();
            if (!avf->data[0]) {
                LOG4CPLUS_WARN_FMT(logger_,
                    "Channel %d: AVFrame data[0] is NULL, skipping", channel_id);
                return false;
            }

            driver_->stitch(decoded, render_buf_,
                           ch_info->layout.x, ch_info->layout.y,
                           ch_info->layout.w, ch_info->layout.h,
                           src_width, src_height,
                           screen_width_, screen_height_,
                           avf->format);
        }
        break;
    }

    // Mark this channel as written, notify render thread
    {
        std::lock_guard<std::mutex> round_lock(round_mutex_);
        ch_info->written_this_round = true;
    }
    render_cv_.notify_one();

    return true;
}

// ============================================================
// Subscriber management
// ============================================================

void FrameStitcherService::subscribe(OnStitchedFrameCallback callback) {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    subscribers_.push_back(std::move(callback));
    LOG4CPLUS_INFO_FMT(logger_, "Subscriber added (total: %d)",
        static_cast<int>(subscribers_.size()));
}

void FrameStitcherService::notifySubscribers(Buffer* buf) {
    std::lock_guard<std::mutex> sub_lock(subscribers_mutex_);
    if (subscribers_.empty()) return;

    StitchedFrame frame;
    frame.buffer = buf;
    frame.width = screen_width_;
    frame.height = screen_height_;
    frame.format = 23; // AV_PIX_FMT_NV12
    frame.data_size = buffer_size_;
    for (auto& cb : subscribers_) {
        cb(frame);
    }
}

// ============================================================
// Display tick (consumes FILLED, notifies subscribers)
// ============================================================

void FrameStitcherService::onTick() {
    if (!running_ || !pool_) return;

    // Restore backpressure: acquire only one buffer per tick (prevents unthrottled rendering loop)
    Buffer* buf = pool_->acquireFilled(false, 0);
    if (!buf) return;

    // Notify all subscribers synchronously
    // (Display subscriber does DMA ioctl, WebUI subscriber does encode)
    notifySubscribers(buf);

    // Release previous displayed buffer, hold current
    if (displayed_buf_) {
        pool_->releaseFilled(displayed_buf_);
    }
    displayed_buf_ = buf;
}

// ============================================================
// Render thread
// ============================================================

void FrameStitcherService::renderThreadFunc() {
    LOG4CPLUS_DEBUG(logger_, "Render thread started");

    if (!pool_) {
        LOG4CPLUS_ERROR(logger_, "Render thread: pool is null");
        return;
    }
    if (!template_buf_) {
        LOG4CPLUS_ERROR(logger_, "Render thread: template_buf is null");
        return;
    }
    if (!driver_) {
        LOG4CPLUS_ERROR(logger_, "Render thread: driver is null");
        return;
    }

    LOG4CPLUS_INFO_FMT(logger_,
        "Render thread ready: screen=%dx%d, template_id=%u, pool_free=%d",
        screen_width_, screen_height_, template_buf_->id(), pool_->getFreeCount());

    while (running_) {
        auto round_start = std::chrono::steady_clock::now();
        // Sleep if no active channels (avoid busy-spinning)
        {
            bool has_active = false;
            {
                std::lock_guard<std::mutex> round_lock(round_mutex_);
                for (const auto& ch : channels_) {
                    if (ch.active) { has_active = true; break; }
                }
            }
            if (!has_active) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
        }

        // (1) Acquire a free buffer from the pool
        Buffer* buf = pool_->acquireFree(true, 100);
        if (!buf) continue;

        // (2) Initialize buf with template frame (full-frame copy)
        LOG4CPLUS_DEBUG_FMT(logger_,
            "Render round %llu: copy template(id=%u) -> buf(id=%u)",
            (unsigned long long)round_seq_ + 1, template_buf_->id(), buf->id());
        driver_->copy(template_buf_.get(), buf, screen_width_, screen_height_);

        // (3) Set render_buf_ so channels can write
        {
            std::unique_lock<std::shared_mutex> lock(rw_mutex_);
            render_buf_ = buf;
        }

        // (4) Increment round counter, reset channel flags, wake channel threads
        {
            std::lock_guard<std::mutex> round_lock(round_mutex_);
            round_seq_++;
            for (auto& ch : channels_) {
                if (ch.active) ch.written_this_round = false;
            }
        }
        round_cv_.notify_all();

        // (5) Wait for all active channels to finish OR frame timeout
        {
            std::unique_lock<std::mutex> round_lock(round_mutex_);
            render_cv_.wait_for(round_lock,
                std::chrono::milliseconds(frame_timeout_ms_),
                [this]() {
                    if (!running_) return true;
                    for (const auto& ch : channels_) {
                        if (ch.active && !ch.written_this_round) return false;
                    }
                    return true;
                });
        }

        if (!running_) {
            pool_->releaseFree(buf);
            break;
        }

        // (6) Disconnect render_buf_ (exclusive lock waits for all in-flight ppResize)
        {
            std::unique_lock<std::shared_mutex> lock(rw_mutex_);
            render_buf_ = nullptr;
        }

        // (7) Check for timed-out channels
        {
            std::lock_guard<std::mutex> round_lock(round_mutex_);
            for (auto& ch : channels_) {
                if (!ch.active) continue;
                if (!ch.written_this_round) {
                    ch.consecutive_misses++;
                    LOG4CPLUS_WARN_FMT(logger_,
                        "Channel %d: missed frame, kept template content (timeout=%dms, consecutive=%d)",
                        ch.channel_id, frame_timeout_ms_, ch.consecutive_misses);
                    if (ch.consecutive_misses == kMaxConsecutiveMisses) {
                        LOG4CPLUS_ERROR_FMT(logger_,
                            "Channel %d: %d consecutive misses (%ds), marking unhealthy",
                            ch.channel_id, ch.consecutive_misses,
                            ch.consecutive_misses * frame_timeout_ms_ / 1000);
                    }
                } else {
                    ch.consecutive_misses = 0;
                }
            }
        }

        // (8) Save current frame to template
        driver_->copy(buf, template_buf_.get(), screen_width_, screen_height_);

        // (9) Submit to FILLED queue — subscriber notification is done
        //     by onDisplayTick (the display timing authority)
        pool_->submitFilled(buf);

        // Limit frame rate to target FPS to prevent GPU/bus overload and deadlock
        int display_interval = 1000 / (config_.target_fps > 0 ? config_.target_fps : 30);
        auto round_end = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(round_end - round_start).count();
        if (elapsed < display_interval) {
            std::this_thread::sleep_for(std::chrono::milliseconds(display_interval - elapsed));
        }
    }

    LOG4CPLUS_DEBUG(logger_, "Render thread exited");
}

// ============================================================
// Template buffer recovery
// ============================================================

std::unique_ptr<Buffer> FrameStitcherService::takeTemplateBuf() {
    return std::move(template_buf_);
}

// ============================================================
// View diagram
// ============================================================

std::string FrameStitcherService::getViewDiagram() const {
    if (view_slots_.empty()) return "(empty view)\n";

    if (view_type_ == ViewType::GRID) {
        int slot_count = static_cast<int>(view_slots_.size());
        int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(slot_count))));
        int rows = (slot_count + cols - 1) / cols;

        auto coord = [](int x, int y) -> std::string {
            return "(" + std::to_string(x) + "," + std::to_string(y) + ")";
        };

        int cell_w = (slot_count > 0) ? view_slots_[0].w : 0;
        int cell_h = (slot_count > 0) ? view_slots_[0].h : 0;

        auto coord_width = [&](int c, int r) -> size_t {
            return coord(c * cell_w, r * cell_h).size();
        };

        size_t max_coord_len = 0;
        for (int r = 0; r <= rows; ++r) {
            for (int c = 0; c <= cols; ++c) {
                max_coord_len = std::max(max_coord_len, coord_width(c, r));
            }
        }

        size_t cell_text_w = std::max(max_coord_len + 4, static_cast<size_t>(16));

        std::ostringstream oss;

        for (int r = 0; r < rows; ++r) {
            // Top border with coordinates
            for (int c = 0; c <= cols; ++c) {
                std::string pt = coord(c * cell_w, r * cell_h);
                oss << pt;
                if (c < cols) {
                    size_t fill = cell_text_w - pt.size();
                    for (size_t i = 0; i < fill; ++i) oss << "\xe2\x94\x80"; // ─
                }
            }
            oss << "\n";

            // Cell content lines
            int slot_base = r * cols;
            for (int line = 0; line < 3; ++line) {
                for (int c = 0; c <= cols; ++c) {
                    oss << "\xe2\x94\x82"; // │
                    if (c < cols) {
                        int slot_idx = slot_base + c;
                        std::string content;
                        if (line == 0) {
                            content = "";
                        } else if (line == 1) {
                            if (slot_idx < slot_count) {
                                int ch_id = slot_idx;
                                if (!slot_assignment_.empty()) {
                                    ch_id = (slot_idx < static_cast<int>(slot_assignment_.size()))
                                            ? slot_assignment_[slot_idx] : -1;
                                }
                                if (ch_id >= 0) {
                                    content = "  [CH " + std::to_string(ch_id) + "]";
                                } else {
                                    content = "  [----]";
                                }
                            }
                        } else if (line == 2) {
                            if (slot_idx < slot_count) {
                                const auto& s = view_slots_[slot_idx];
                                content = "  " + std::to_string(s.w) + "x" + std::to_string(s.h);
                            }
                        }
                        size_t pad = (content.size() < cell_text_w) ? cell_text_w - content.size() : 0;
                        oss << content << std::string(pad, ' ');
                    }
                }
                oss << "\n";
            }
        }

        // Bottom border
        for (int c = 0; c <= cols; ++c) {
            std::string pt = coord(c * cell_w, rows * cell_h);
            oss << pt;
            if (c < cols) {
                size_t fill = cell_text_w - pt.size();
                for (size_t i = 0; i < fill; ++i) oss << "\xe2\x94\x80"; // ─
            }
        }
        oss << "\n";

        return oss.str();

    } else {
        // MAIN_SIDEBAR
        const auto& main_slot = view_slots_[0];
        int main_w = main_slot.w;
        int side_w = (view_slots_.size() > 1) ? view_slots_[1].w : 0;
        int side_h = (view_slots_.size() > 1) ? view_slots_[1].h : 0;

        auto coord = [](int x, int y) -> std::string {
            return "(" + std::to_string(x) + "," + std::to_string(y) + ")";
        };

        size_t main_col_w = 32;
        size_t side_col_w = 18;

        std::ostringstream oss;

        // Top border
        std::string tl = coord(0, 0);
        std::string tm = coord(main_w, 0);
        std::string tr = coord(main_w + side_w, 0);
        oss << tl;
        for (size_t i = tl.size(); i < main_col_w; ++i) oss << "\xe2\x94\x80";
        oss << tm;
        for (size_t i = tm.size(); i < side_col_w; ++i) oss << "\xe2\x94\x80";
        oss << tr << "\n";

        int main_ch_id = 0;
        if (!slot_assignment_.empty()) {
            main_ch_id = slot_assignment_[0];
        }

        for (int i = 0; i < 4; ++i) {
            int ch_id = 1 + i;
            if (!slot_assignment_.empty() && (1 + i) < static_cast<int>(slot_assignment_.size())) {
                ch_id = slot_assignment_[1 + i];
            }

            for (int line = 0; line < 3; ++line) {
                // Left column (main panel)
                oss << "\xe2\x94\x82"; // │
                std::string left_content;
                if (i == 1 && line == 1) {
                    left_content = "      [CH " + std::to_string(main_ch_id) + "]";
                } else if (i == 2 && line == 1) {
                    left_content = "      " + std::to_string(main_w) + "x" + std::to_string(screen_height_);
                }
                size_t lpad = (left_content.size() < main_col_w - 1) ? main_col_w - 1 - left_content.size() : 0;
                oss << left_content << std::string(lpad, ' ');

                // Right column (sidebar channel)
                oss << "\xe2\x94\x82"; // │
                std::string right_content;
                if (line == 1) {
                    right_content = " [CH " + std::to_string(ch_id) + "] " +
                                    std::to_string(side_w) + "x" + std::to_string(side_h);
                }
                size_t rpad = (right_content.size() < side_col_w - 1) ? side_col_w - 1 - right_content.size() : 0;
                oss << right_content << std::string(rpad, ' ');

                oss << "\xe2\x94\x82\n"; // │
            }

            // Sidebar separator
            if (i < 3) {
                oss << "\xe2\x94\x82"; // │
                oss << std::string(main_col_w - 1, ' ');
                std::string mid_pt = coord(main_w, (i + 1) * side_h);
                oss << mid_pt;
                for (size_t j = mid_pt.size(); j < side_col_w; ++j) oss << "\xe2\x94\x80";
                oss << coord(main_w + side_w, (i + 1) * side_h) << "\n";
            }
        }

        // Bottom border
        std::string bl = coord(0, screen_height_);
        std::string bm = coord(main_w, screen_height_);
        std::string br = coord(main_w + side_w, screen_height_);
        oss << bl;
        for (size_t i = bl.size(); i < main_col_w; ++i) oss << "\xe2\x94\x80";
        oss << bm;
        for (size_t i = bm.size(); i < side_col_w; ++i) oss << "\xe2\x94\x80";
        oss << br << "\n";

        return oss.str();
    }
}
