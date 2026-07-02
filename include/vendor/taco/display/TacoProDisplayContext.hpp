#ifndef TACO_PRO_DISPLAY_CONTEXT_HPP
#define TACO_PRO_DISPLAY_CONTEXT_HPP

#include "bufferpool/buffer/Buffer.hpp"
#include "bufferpool/pool/base/BufferPool.hpp"
#include "bufferpool/pool/base/IBufferPoolBuilder.hpp"
#include "bufferpool/pool/builder/BufferPoolBuilderFactory.hpp"
#include "productionline/worker/base/ComponentTopology.hpp"
#include "vendor/taco/display/TacoProDisplayExtension.hpp"
#include "consumptionline/types/stitcher/FrameStitcherService.hpp"

#include <mutex>
#include <memory>
#include <string>
#include <cstdint>



#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

struct tpsfb_dma_info;
class TacoProOsdOverlay;

/**
 * TacoProDisplayContext - 多通道共享显示上下文（单例模式）
 *
 * 核心职责：
 *   1. 通过 TACO 平台 API 独立分配每个 framebuffer 页的物理连续内存
 *   2. 通过 IBufferPoolBuilder（CONTINUOUS_PHYSICAL 类型）+ BufferPool 管理 framebuffer 页
 *   3. 通过 FrameStitcherService 管理多通道拼接、渲染和定时消费
 *   4. 作为 Display 订阅者注册到 FrameStitcherService，收到帧时执行 DMA 送显
 *
 * 内存分配策略：
 *   - 每个 buffer 由 FramebufferAllocator 内部独立分配物理连续内存
 *   - 每帧显示时通过 FB_IOCTL_SET_DMA_INFO 动态设置 DMA 基地址
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
    static std::shared_ptr<TacoProDisplayContext> getInstance() {
        std::lock_guard<std::mutex> lock(s_acquire_mutex_);
        return s_instance_.lock();
    }

    ~TacoProDisplayContext();

    TacoProDisplayContext(const TacoProDisplayContext&) = delete;
    TacoProDisplayContext& operator=(const TacoProDisplayContext&) = delete;

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

    ViewType getViewType() const;
    int getSlotCount() const;
    const ChannelLayout& getSlotLayout(int slot_index) const;

    /**
     * 获取当前视图的 ASCII 示意图（坐标标注在网格交叉点）
     * 用于日志输出 / 调试 / 展示给用户确认布局
     */
    std::string getViewDiagram() const;

    /**
     * 获取 stitcher 服务（用于外部订阅拼接帧）
     */
    std::shared_ptr<FrameStitcherService> getStitcher() const { return stitcher_; }

private:
    explicit TacoProDisplayContext(const TacoProDisplayExtension& config);
    bool open();
    void close();

    bool openDevice();
    bool createBufferPool();

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
    std::unique_ptr<IBufferPoolBuilder> builder_;
    uint64_t fb_pool_id_ = 0;

    std::shared_ptr<BufferPool> getBufferPool();

    // === 模板帧（专用 TACO 内存，用于跨帧继承）===
    uint32_t template_blk_id_ = 0;

    // === Stitcher 服务 ===
    std::shared_ptr<FrameStitcherService> stitcher_;

    // === OSD 叠加（可选，图形层 overlay1）===
    std::unique_ptr<TacoProOsdOverlay> osd_;

    // === 单例 ===
    static std::mutex s_acquire_mutex_;
    static std::weak_ptr<TacoProDisplayContext> s_instance_;

    log4cplus::Logger logger_;
};

#endif // TACO_PRO_DISPLAY_CONTEXT_HPP
