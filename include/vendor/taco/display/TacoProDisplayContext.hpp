#ifndef TACO_PRO_DISPLAY_CONTEXT_HPP
#define TACO_PRO_DISPLAY_CONTEXT_HPP

#include "buffer/bufferpool/Buffer.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "buffer/BufferAllocatorFacade.hpp"
#include "buffer/BufferAllocatorFactory.hpp"
#include "productionline/worker/base/ComponentTopology.hpp"
#include "vendor/taco/display/TacoProDisplayExtension.hpp"

#include <shared_mutex>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <memory>
#include <string>
#include <cstdint>

#include "common/Timer.hpp"

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

extern "C" {
#include "ta_cv_api_ext_c.h"
}

enum class ViewType { GRID, MAIN_SIDEBAR };

struct tpsfb_dma_info;
class TacoProOsdOverlay;

/**
 * TacoProDisplayContext - 多通道共享显示上下文（单例模式）
 *
 * 核心职责：
 *   1. 通过 TACO 平台 API 独立分配每个 framebuffer 页的物理连续内存
 *   2. 通过 BufferAllocatorFacade（FRAMEBUFFER 类型）+ BufferPool 管理 framebuffer 页
 *   3. 提供 channelWrite() 接口供多通道并发写入（PP 硬件 resize）
 *   4. 渲染线程等待所有通道写完后提交（帧级超时保护）
 *   5. 显示定时器定时从 FILLED 队列取帧 → DMA → VSYNC
 *
 * 内存分配策略：
 *   - 每个 buffer 由 FramebufferAllocator 内部独立分配物理连续内存
 *   - 每帧显示时通过 FB_IOCTL_SET_DMA_INFO 动态设置 DMA 基地址
 *
 * 同步机制：
 *   - std::shared_mutex：通道获取 shared_lock 并发写入，渲染线程获取 unique_lock 切换 buffer
 *   - render_cv_：通道写完后通知渲染线程，渲染线程等待所有通道完成或帧超时
 *   - round_cv_：渲染线程新一轮开始时唤醒等待的通道线程
 *
 * 生命周期：
 *   - 通过 acquire() 获取 shared_ptr（内部 weak_ptr 单例）
 *   - 第一个调用者创建，最后一个释放时自动销毁
 */
class TacoProDisplayContext {
public:
    /**
     * 获取共享上下文实例（单例模式）
     * 第一个调用者创建实例，后续调用者复用
     */
    static std::shared_ptr<TacoProDisplayContext> acquire(const TacoProDisplayExtension& config);

    ~TacoProDisplayContext();

    TacoProDisplayContext(const TacoProDisplayContext&) = delete;
    TacoProDisplayContext& operator=(const TacoProDisplayContext&) = delete;

    struct ChannelLayout {
        int x;
        int y;
        int w;
        int h;
    };

    /**
     * 注册一个显示通道（自动计算网格布局）
     * @return channel_id (0-based)，失败返回 -1
     */
    int registerChannel();

    /**
     * 注册一个显示通道（手动指定布局）
     * @param layout 通道在屏幕上的位置和尺寸
     * @return channel_id (0-based)，失败返回 -1
     */
    int registerChannel(const ChannelLayout& layout);

    /**
     * 注销一个显示通道
     */
    void unregisterChannel(int channel_id);

    /**
     * 通道写入：将解码帧通过 PP resize 写入当前 render buffer 的对应区域
     *
     * 线程安全：多通道可并发调用（shared_lock）
     *
     * @param channel_id  通道 ID
     * @param decoded     解码帧 Buffer（需有物理地址和图像元数据）
     * @return true 写入成功，false 定时器正在切换，调用方应缓存此帧
     */
    bool channelWrite(int channel_id, Buffer* decoded);

    int getScreenWidth() const { return screen_width_; }
    int getScreenHeight() const { return screen_height_; }
    int getBitsPerPixel() const { return bits_per_pixel_; }

    ViewType getViewType() const { return view_type_; }
    int getSlotCount() const { return static_cast<int>(view_slots_.size()); }
    const ChannelLayout& getSlotLayout(int slot_index) const;

    /**
     * 获取当前视图的 ASCII 示意图（坐标标注在网格交叉点）
     * 用于日志输出 / 调试 / 展示给用户确认布局
     */
    std::string getViewDiagram() const;

private:
    explicit TacoProDisplayContext(const TacoProDisplayExtension& config);
    bool open();
    void close();

    bool openDevice();
    bool createBufferPool();
    bool startThreads();
    void stopThreads();

    void ppResize(Buffer* src, Buffer* dst,
                  int dst_x, int dst_y, int dst_w, int dst_h,
                  int src_width, int src_height,
                  uint64_t src_phys, int src_format, const int* src_linesize);
    void ppCopy(Buffer* src, Buffer* dst);
    void copyTemplateRegion(Buffer* dst, const ChannelLayout& layout);

    void createView();
    const ChannelLayout& resolveLayout(int channel_id) const;

    void renderThreadFunc();
    void onDisplayTick();

    // === 配置 ===
    TacoProDisplayExtension config_;

    // === Framebuffer 设备 ===
    int fd_;
    int fb_index_;
    int screen_width_;
    int screen_height_;
    int bits_per_pixel_;
    size_t buffer_size_;
    int buffer_count_;

    // === BufferPool 管理 ===
    // allocatePoolWithBuffers 内部通过 TACO API 分配，destroyPool 自动清理
    std::unique_ptr<BufferAllocatorFacade> allocator_facade_;
    uint64_t fb_pool_id_ = 0;

    std::shared_ptr<BufferPool> getBufferPool();

    // === 渲染状态 ===
    Buffer* render_buf_;

    // === 显示状态（当前正被显示控制器扫描的帧，延后到下一 tick 释放）===
    Buffer* displayed_buf_ = nullptr;

    // === 模板帧（专用 TACO 内存，用于跨帧继承）===
    std::unique_ptr<Buffer> template_buf_;
    uint32_t template_blk_id_ = 0;

    // === 同步原语 ===
    std::shared_mutex rw_mutex_;

    // === 视图管理 ===
    ViewType view_type_ = ViewType::GRID;
    std::vector<ChannelLayout> view_slots_;     // 预计算的所有 slot 布局
    std::vector<int> slot_assignment_;           // slot_assignment_[slot_index] = channel_id

    // === 通道管理 ===
    struct ChannelInfo {
        int channel_id;
        ChannelLayout layout;
        bool active;
        bool written_this_round = false;
        int consecutive_misses = 0;
    };
    std::vector<ChannelInfo> channels_;
    std::mutex channel_mgmt_mutex_;
    int next_channel_id_ = 0;

    // === 通道写入节流（条件变量）===
    std::mutex round_mutex_;
    std::condition_variable round_cv_;        // 渲染线程 → 通道线程：新一轮开始
    std::condition_variable render_cv_;       // 通道线程 → 渲染线程：写入完成
    uint64_t round_seq_ = 0;                 // 轮次计数器，每轮递增

    // === 线程 & 定时器 ===
    Timer timer_;
    Timer::TimerId timer_id_ = 0;
    std::thread render_thread_;
    std::atomic<bool> running_{false};
    int frame_timeout_ms_ = 33;
    static constexpr int kMaxConsecutiveMisses = 90;

    // === OSD 叠加（可选，图形层 overlay1）===
    std::unique_ptr<TacoProOsdOverlay> osd_;

    // === 单例 ===
    static std::mutex s_acquire_mutex_;
    static std::weak_ptr<TacoProDisplayContext> s_instance_;

    log4cplus::Logger logger_;
};

#endif // TACO_PRO_DISPLAY_CONTEXT_HPP
