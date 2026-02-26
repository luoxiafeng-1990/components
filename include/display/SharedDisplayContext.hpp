#ifndef SHARED_DISPLAY_CONTEXT_HPP
#define SHARED_DISPLAY_CONTEXT_HPP

#include "buffer/bufferpool/Buffer.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "buffer/BufferAllocatorFacade.hpp"
#include "buffer/BufferAllocatorFactory.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include "productionline/worker/WorkerConfig.hpp"

#include <shared_mutex>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <memory>
#include <cstdint>

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

extern "C" {
#include "taco_sys_api.h"
#include "ta_cv_api_ext_c.h"
}

struct tpsfb_dma_info;

/**
 * SharedDisplayContext - 多通道共享显示上下文（单例模式）
 *
 * 核心职责：
 *   1. 管理 DMA 物理连续内存（taco_sys_get_block 分配的 framebuffer）
 *   2. 用 BufferPool 管理 4 个 framebuffer 页（FREE/FILLED 队列）
 *   3. 提供 channelWrite() 接口供多通道并发写入（PP 硬件 resize）
 *   4. timerfd 定时器驱动翻页（与通道解码速度解耦）
 *   5. 显示线程负责 FBIOPAN + VSYNC
 *
 * 同步机制：
 *   - std::shared_mutex：通道获取 shared_lock 并发写入，定时器获取 unique_lock 切换 buffer
 *   - 保证 PP resize 一定完成后定时器才切换
 *   - 通道在定时器切换期间不阻塞，看到 nullptr 直接返回 false（调用方缓存帧）
 *
 * 生命周期：
 *   - 通过 acquire() 获取 shared_ptr（内部 weak_ptr 单例）
 *   - 第一个调用者创建，最后一个释放时自动销毁
 */
class SharedDisplayContext {
public:
    using TacoVOConfig = WorkerConfig::ConsumerTypeConfig::DisplayType::TacoVOConfig;

    /**
     * 获取共享上下文实例（单例模式）
     * 第一个调用者创建实例，后续调用者复用
     */
    static std::shared_ptr<SharedDisplayContext> acquire(const TacoVOConfig& config);

    ~SharedDisplayContext();

    SharedDisplayContext(const SharedDisplayContext&) = delete;
    SharedDisplayContext& operator=(const SharedDisplayContext&) = delete;

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

private:
    explicit SharedDisplayContext(const TacoVOConfig& config);
    bool init();
    void shutdown();

    bool openFramebufferDevice();
    bool allocateDmaMemory();
    void freeDmaMemory();
    bool setupDssForDma();
    bool createFramebufferPool();
    bool startThreads();
    void stopThreads();

    void ppResize(Buffer* src, Buffer* dst,
                  int dst_x, int dst_y, int dst_w, int dst_h,
                  int src_width, int src_height,
                  uint64_t src_phys, int src_format, const int* src_linesize);
    void ppCopy(Buffer* src, Buffer* dst);
    void computeGridLayout(int channel_index, ChannelLayout& layout) const;

    void timerThreadFunc();
    void displayThreadFunc();

    // === 配置 ===
    TacoVOConfig config_;

    // === Framebuffer 设备 ===
    int fd_;
    int fb_index_;
    int screen_width_;
    int screen_height_;
    int bits_per_pixel_;
    size_t buffer_size_;
    int buffer_count_;

    // === DMA 物理连续内存 ===
    struct DmaMemory {
        uint32_t blk_id = 0;
        uint64_t phys_addr = 0;
        void*    virt_addr = nullptr;
        size_t   total_size = 0;
    };
    DmaMemory dma_mem_;

    // === 用于 ta_cv_image_create 的 metadata（PP 硬件需要 blk_id 查找物理地址）===
    char blk_id_str_[32] = {};
    TA_AVDictionaryEntry dict_entry_ = {};
    TA_AVDictionary dict_ = {};

    // === BufferPool 管理 framebuffer 页 ===
    std::unique_ptr<BufferAllocatorFacade> allocator_;
    uint64_t fb_pool_id_ = 0;

    std::shared_ptr<BufferPool> getPool();

    // === 渲染/显示状态 ===
    Buffer* render_buf_;
    Buffer* display_buf_;

    // === 同步原语 ===
    std::shared_mutex rw_mutex_;

    // === 通道管理 ===
    struct ChannelInfo {
        int  channel_id;
        int  x, y, w, h;
        bool active;
        bool written_this_round = false;
    };
    std::vector<ChannelInfo> channels_;
    std::mutex channel_mgmt_mutex_;
    int next_channel_id_ = 0;

    // === 通道写入节流（条件变量）===
    std::mutex round_mutex_;
    std::condition_variable round_cv_;

    // === display_buf_ 读写保护 ===
    std::mutex display_mutex_;

    // === 线程 ===
    std::thread timer_thread_;
    std::thread display_thread_;
    std::atomic<bool> running_{false};
    int timer_fd_ = -1;

    // === 单例 ===
    static std::mutex s_acquire_mutex_;
    static std::weak_ptr<SharedDisplayContext> s_instance_;

    log4cplus::Logger logger_;
};

#endif // SHARED_DISPLAY_CONTEXT_HPP
