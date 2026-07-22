#ifndef FRAME_STITCHER_SERVICE_HPP
#define FRAME_STITCHER_SERVICE_HPP

#include "consumptionline/types/stitcher/IFrameStitcherDriver.hpp"
#include "bufferpool/buffer/Buffer.hpp"
#include "bufferpool/pool/base/BufferPool.hpp"
#include "common/Timer.hpp"

#include <shared_mutex>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <memory>
#include <string>
#include <cstdint>
#include <functional>

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

// ============================================================
// Shared types (used by both FrameStitcherService and callers)
// ============================================================

enum class ViewType { GRID, MAIN_SIDEBAR };

struct ChannelLayout {
    int x;
    int y;
    int w;
    int h;
};

/**
 * @brief One slot in a stitcher layout snapshot (IDS grid / main_sidebar).
 *
 * Geometry always comes from view_slots_. channel_id / worker_id are optional
 * mappings established at DISPLAY register time (worker_id may be empty for QA).
 */
struct LayoutSlotSnapshot {
    int slot = 0;
    int channel_id = -1;   ///< -1 if no channel assigned to this slot
    std::string worker_id; ///< empty if unknown / QA path
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

/**
 * @brief Durable layout view for GET /api/preview/layout (spec §13.1).
 *
 * Built from view_slots_ / slot_assignment_, not from Worker list order.
 */
struct LayoutSnapshot {
    int width = 0;
    int height = 0;
    int rows = 0;
    int cols = 0;
    std::string view_type = "grid";
    std::vector<LayoutSlotSnapshot> slots;
};

/**
 * @brief Configuration for FrameStitcherService.
 */
struct FrameStitcherConfig {
    int screen_width = 1920;
    int screen_height = 1080;
    int bits_per_pixel = 12;
    int target_fps = 30;
    std::string view_type = "grid";
    std::vector<int> slot_assignment;
    float main_sidebar_ratio = 0.75f;
};

/**
 * @brief Describes a stitched (composited) frame ready for consumption.
 *
 * Passed to subscriber callbacks after each render-thread round completes.
 */
struct StitchedFrame {
    Buffer* buffer = nullptr;      ///< The stitched render buffer (read-only for subscribers!)
    int width = 0;                 ///< Screen width
    int height = 0;                ///< Screen height
    int format = 23;               ///< Pixel format (AV_PIX_FMT_NV12 = 23)
    size_t data_size = 0;          ///< Total buffer size in bytes
};

using OnStitchedFrameCallback = std::function<void(const StitchedFrame&)>;

/**
 * @brief FrameStitcherService — multi-channel frame compositor.
 *
 * Extracted from TacoProDisplayContext to decouple stitching logic from
 * display hardware management. The service:
 *   1. Manages channel registration / layout computation
 *   2. Provides channelWrite() for concurrent per-channel frame input
 *   3. Runs a render thread that waits for all channels, then submits
 *      the composited frame to the BufferPool FILLED queue
 *   4. Runs an onTick timer that consumes from FILLED and notifies
 *      all subscribers (Display DMA, WebUI encoding, etc.)
 *
 * Thread-safety: channelWrite() is safe for concurrent calls from
 * multiple decoder threads (uses shared_lock). The render thread
 * obtains unique_lock when swapping the render buffer.
 */
class FrameStitcherService : public std::enable_shared_from_this<FrameStitcherService> {
public:
    static std::shared_ptr<FrameStitcherService> getInstance();
    /**
     * @param config        Stitcher configuration (screen dims, fps, view type, etc.)
     * @param driver        Stitch / copy driver (hardware or software)
     * @param pool          BufferPool that owns the render buffers
     * @param template_buf  Pre-allocated template buffer (NV12 black frame)
     */
    FrameStitcherService(const FrameStitcherConfig& config,
                         std::shared_ptr<IFrameStitcherDriver> driver,
                         std::shared_ptr<BufferPool> pool,
                         std::unique_ptr<Buffer> template_buf);

    ~FrameStitcherService();

    FrameStitcherService(const FrameStitcherService&) = delete;
    FrameStitcherService& operator=(const FrameStitcherService&) = delete;

    // === Lifecycle ===

    bool start();
    void stop();

    // === Channel management ===

    /**
     * Register a channel with auto-computed grid layout.
     * @return channel_id (>= 0), or -1 on failure
     */
    int registerChannel();

    /**
     * Register a channel with an explicit layout.
     * @return channel_id (>= 0), or -1 on failure
     */
    int registerChannel(const ChannelLayout& layout);

    /**
     * Unregister a channel.
     */
    void unregisterChannel(int channel_id);

    /**
     * Persist optional WebUI worker_id for a registered channel.
     * Empty worker_id clears the mapping (QA Display path may leave it empty).
     */
    void setChannelWorkerId(int channel_id, const std::string& worker_id);

    /**
     * Snapshot of current view geometry + channel/worker mapping.
     * Slot order follows view_slots_ (IDS grid), never workers[] order.
     */
    LayoutSnapshot getLayoutSnapshot();

    // === Frame input ===

    /**
     * Write a decoded frame into this channel's region of the current render buffer.
     *
     * Thread-safe: multiple channels can call concurrently (shared_lock).
     *
     * @param channel_id  Channel ID (from registerChannel)
     * @param decoded     Decoded frame buffer (must have AVFrame payload)
     * @return true if written, false if channel inactive or shutting down
     */
    bool channelWrite(int channel_id, Buffer* decoded);

    // === Subscriber (e.g. WebUI streaming) ===

    /**
     * Subscribe to stitched frame notifications.
     * Callback is invoked on the render thread after compositing completes,
     * just before the buffer is submitted to the pool's FILLED queue.
     */
    void subscribe(OnStitchedFrameCallback callback);

    // === Accessors ===

    int getScreenWidth()  const { return screen_width_; }
    int getScreenHeight() const { return screen_height_; }
    ViewType getViewType() const { return view_type_; }
    int getSlotCount() const { return static_cast<int>(view_slots_.size()); }
    const ChannelLayout& getSlotLayout(int slot_index) const;

    /**
     * ASCII diagram of the current view layout (for logging / debugging).
     */
    std::string getViewDiagram() const;

    /**
     * Retrieve the template buffer (e.g. for recovery on shutdown).
     * Transfers ownership to the caller; internal pointer becomes null.
     */
    std::unique_ptr<Buffer> takeTemplateBuf();

private:
    // === Internal helpers ===

    void createView();
    const ChannelLayout& resolveLayout(int channel_id) const;
    void renderThreadFunc();
    void onTick();
    void notifySubscribers(Buffer* buf);

    // === Configuration ===
    int screen_width_;
    int screen_height_;
    int bits_per_pixel_;
    size_t buffer_size_;
    int frame_timeout_ms_ = 33;
    static constexpr int kMaxConsecutiveMisses = 90;

    // === View ===
    ViewType view_type_ = ViewType::GRID;
    std::vector<ChannelLayout> view_slots_;
    std::vector<int> slot_assignment_;
    FrameStitcherConfig config_;

    // === Channel state ===
    struct ChannelInfo {
        int channel_id;
        ChannelLayout layout;
        bool active;
        bool written_this_round = false;
        int consecutive_misses = 0;
        std::string worker_id;  ///< Optional; set via setChannelWorkerId
    };
    std::vector<ChannelInfo> channels_;
    std::mutex channel_mgmt_mutex_;
    int next_channel_id_ = 0;

    // === Synchronisation ===
    std::shared_mutex rw_mutex_;
    std::mutex round_mutex_;
    std::condition_variable round_cv_;
    std::condition_variable render_cv_;
    uint64_t round_seq_ = 0;

    // === Render state ===
    Buffer* render_buf_ = nullptr;
    std::unique_ptr<Buffer> template_buf_;
    std::thread render_thread_;
    std::atomic<bool> running_{false};

    // === Display tick (consumes FILLED, notifies subscribers) ===
    Buffer* displayed_buf_ = nullptr;
    Timer timer_;
    Timer::TimerId timer_id_ = 0;

    // === Dependencies ===
    std::shared_ptr<IFrameStitcherDriver> driver_;
    std::shared_ptr<BufferPool> pool_;

    // === Subscribers ===
    std::mutex subscribers_mutex_;
    std::vector<OnStitchedFrameCallback> subscribers_;

    log4cplus::Logger logger_;

    // === Singleton/Active Instance ===
    static std::mutex s_instance_mutex;
    static std::weak_ptr<FrameStitcherService> s_active_instance;
};

#endif // FRAME_STITCHER_SERVICE_HPP
