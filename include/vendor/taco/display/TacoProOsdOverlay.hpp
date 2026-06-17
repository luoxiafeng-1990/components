#ifndef TACO_PRO_OSD_OVERLAY_HPP
#define TACO_PRO_OSD_OVERLAY_HPP

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <memory>

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "common/Timer.hpp"
#include "bufferpool/pool/base/IBufferPoolBuilder.hpp"
#include "bufferpool/pool/builder/BufferPoolBuilderFactory.hpp"
#include "bufferpool/pool/base/BufferPool.hpp"
#include "productionline/worker/base/ComponentTopology.hpp"

/**
 * TacoProOsdOverlay - vendor=tacopro 图形层 OSD 叠加显示
 *
 * 在 DSS overlay1 上渲染通道号、时间戳、帧率等文字信息。
 * 使用 ARGB8888 格式，alpha=0 的区域完全透明，不遮挡视频层。
 * 通过 FramebufferAllocator + BufferPool 管理 DMA 缓冲页，每帧动态设置 DMA 基地址。
 */
class TacoProOsdOverlay {
public:
    struct Config {
        int  refresh_fps = 1;
        std::string font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
        int  font_size = 24;
    };

    TacoProOsdOverlay(int screen_width, int screen_height, int max_channels);
    ~TacoProOsdOverlay();

    TacoProOsdOverlay(const TacoProOsdOverlay&) = delete;
    TacoProOsdOverlay& operator=(const TacoProOsdOverlay&) = delete;

    bool init(const Config& config);
    void shutdown();

    void registerChannel(int channel_id, int x, int y, int w, int h);
    void unregisterChannel(int channel_id);

    void recordFrame(int channel_id);

private:
    struct OsdChannelInfo {
        int channel_id = -1;
        int x = 0, y = 0, w = 0, h = 0;
        bool active = false;
        std::atomic<int> frame_count;
        double current_fps = 0.0;

        OsdChannelInfo() : frame_count(0) {}
        OsdChannelInfo(const OsdChannelInfo& o)
            : channel_id(o.channel_id), x(o.x), y(o.y), w(o.w), h(o.h)
            , active(o.active), frame_count(o.frame_count.load())
            , current_fps(o.current_fps) {}
        OsdChannelInfo& operator=(const OsdChannelInfo& o) {
            if (this != &o) {
                channel_id = o.channel_id; x = o.x; y = o.y; w = o.w; h = o.h;
                active = o.active; frame_count.store(o.frame_count.load());
                current_fps = o.current_fps;
            }
            return *this;
        }
    };

    static constexpr int BUFFER_COUNT = 4;

    bool openFbDevice();
    bool createBufferPool();
    bool setupDssOverlay1();
    bool initFreeType(const Config& config);
    void cleanupFreeType();

    void onTimerTick();
    void renderOsd();
    void drawText(int x, int y, const std::string& text, uint32_t color, uint32_t* buf);
    void drawCharGlyph(int base_x, int base_y, FT_GlyphSlot glyph, uint32_t color, uint32_t* buf);

    int screen_width_;
    int screen_height_;
    int max_channels_;
    size_t frame_size_ = 0;

    int fb_fd_ = -1;

    std::unique_ptr<IBufferPoolBuilder> allocator_;
    uint64_t pool_id_ = 0;
    Buffer* display_buf_ = nullptr;

    FT_Library ft_lib_ = nullptr;
    FT_Face    ft_face_ = nullptr;

    std::vector<OsdChannelInfo> channels_;
    std::mutex channel_mutex_;

    Timer timer_;
    Timer::TimerId timer_id_ = 0;
    Config config_;

    log4cplus::Logger logger_;
};

#endif // TACO_PRO_OSD_OVERLAY_HPP
