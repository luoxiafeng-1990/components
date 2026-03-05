#include "display/OsdOverlay.hpp"
#include "common/Logger.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <cstdio>
#include <cmath>
#include <algorithm>

#define PROC_FB "/proc/fb"
#define TPS_FB0 "tpsfb0"

static const char* OVL1_ENABLED_PATH =
    "/sys/devices/platform/soc/soc:dss@c9200000/dss-overlay1/enabled";
static const char* OVL1_PIX_FMT_PATH =
    "/sys/devices/platform/soc/soc:dss@c9200000/dss-overlay1/pixel_fmt";
static const char* LCD_TRANS_KEY_VALUE_PATH =
    "/sys/devices/platform/soc/soc:dss@c9200000/manager-lcd/trans_key_value";
static const char* LCD_TRANS_KEY_ENABLED_PATH =
    "/sys/devices/platform/soc/soc:dss@c9200000/manager-lcd/trans_key_enabled";
static const char* LCD_ALPHA_BLEND_PATH =
    "/sys/devices/platform/soc/soc:dss@c9200000/manager-lcd/alpha_blending_enabled";

struct tpsfb_dma_info {
    int ovl_idx;
    unsigned long long phys_addr;
};
#define FB_IOCTL_SET_DMA_INFO _IOW('F', 7, struct tpsfb_dma_info)

static void write_sysfs(const char* path, const char* value) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s", value);
    fclose(f);
}

// ============================================================
// 构造 / 析构
// ============================================================

OsdOverlay::OsdOverlay(int screen_width, int screen_height, int max_channels)
    : screen_width_(screen_width)
    , screen_height_(screen_height)
    , max_channels_(max_channels)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Display.OSD")))
{
    channels_.resize(max_channels);
}

OsdOverlay::~OsdOverlay() {
    shutdown();
}

// ============================================================
// 初始化 / 关闭
// ============================================================

bool OsdOverlay::init(const Config& config) {
    config_ = config;

    if (!openFbDevice()) return false;
    if (!allocateDmaMemory()) { close(fb_fd_); fb_fd_ = -1; return false; }
    if (!createBufferPool()) { freeDmaMemory(); close(fb_fd_); fb_fd_ = -1; return false; }
    if (!setupDssOverlay1()) { freeDmaMemory(); close(fb_fd_); fb_fd_ = -1; return false; }
    if (!initFreeType(config)) { freeDmaMemory(); close(fb_fd_); fb_fd_ = -1; return false; }

    int fps = config_.refresh_fps > 0 ? config_.refresh_fps : 1;
    int interval_ms = 1000 / fps;

    timer_.start();
    timer_id_ = timer_.scheduleRepeated(interval_ms, [this]() { onTimerTick(); });

    LOG4CPLUS_INFO_FMT(logger_,
        "OSD initialized: %dx%d, %d buffers (BufferPool), refresh=%dfps, font=%s size=%d",
        screen_width_, screen_height_, BUFFER_COUNT, fps,
        config_.font_path.c_str(), config_.font_size);
    return true;
}

void OsdOverlay::shutdown() {
    if (timer_id_ != 0) {
        timer_.cancel(timer_id_);
        timer_id_ = 0;
    }
    timer_.stop();

    write_sysfs(OVL1_ENABLED_PATH, "0");
    write_sysfs(LCD_TRANS_KEY_ENABLED_PATH, "0");

    cleanupFreeType();

    if (pool_id_ != 0) {
        auto pool = BufferPoolRegistry::getInstance().getPool(pool_id_).lock();
        if (pool) {
            if (display_buf_) {
                pool->releaseFilled(display_buf_);
                display_buf_ = nullptr;
            }
            pool->shutdown();
        }
        pool_id_ = 0;
    }
    allocator_.reset();
    display_buf_ = nullptr;

    freeDmaMemory();

    if (fb_fd_ >= 0) { close(fb_fd_); fb_fd_ = -1; }

    LOG4CPLUS_INFO(logger_, "OSD shutdown complete");
}

// ============================================================
// 打开 fb 设备（查找 tpsfb0 对应的 fb 编号 +1 即为 overlay1）
// ============================================================

bool OsdOverlay::openFbDevice() {
    FILE* fp = fopen(PROC_FB, "r");
    if (!fp) {
        LOG4CPLUS_ERROR_FMT(logger_, "Cannot open %s: %s", PROC_FB, strerror(errno));
        return false;
    }

    int fb_num = -1;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        int num; char name[64];
        if (sscanf(line, "%d %63s", &num, name) == 2) {
            if (strcmp(name, TPS_FB0) == 0) {
                fb_num = num + 1;
                break;
            }
        }
    }
    fclose(fp);

    if (fb_num < 0) {
        LOG4CPLUS_ERROR(logger_, "tpsfb0 not found in /proc/fb");
        return false;
    }

    char fb_path[32];
    snprintf(fb_path, sizeof(fb_path), "/dev/fb%d", fb_num);

    fb_fd_ = open(fb_path, O_RDWR);
    if (fb_fd_ < 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "Cannot open %s: %s", fb_path, strerror(errno));
        return false;
    }

    struct fb_var_screeninfo var_info;
    if (ioctl(fb_fd_, FBIOGET_VSCREENINFO, &var_info) == 0) {
        LOG4CPLUS_INFO_FMT(logger_, "OSD fb device: %s, resolution=%dx%d, bpp=%d, yres_virtual=%d",
            fb_path, var_info.xres, var_info.yres,
            var_info.bits_per_pixel, var_info.yres_virtual);

        if (static_cast<int>(var_info.xres) != screen_width_ ||
            static_cast<int>(var_info.yres) != screen_height_) {
            LOG4CPLUS_WARN_FMT(logger_,
                "OSD fb resolution %dx%d != screen %dx%d, check device tree overlay-1 resolution",
                var_info.xres, var_info.yres, screen_width_, screen_height_);
        }
    }

    return true;
}

// ============================================================
// DMA 内存分配（4 帧）
// ============================================================

bool OsdOverlay::allocateDmaMemory() {
    size_t frame_size = static_cast<size_t>(screen_width_) * screen_height_ * 4;
    size_t total_size = frame_size * BUFFER_COUNT;

    uint32_t blk_id = taco_sys_get_block(TACO_INVALID_POOLID, total_size, "osd_overlay1");
    if (blk_id == 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "OSD taco_sys_get_block failed, size=%zu", total_size);
        return false;
    }

    uint64_t phys = taco_sys_handle2_phys_addr(blk_id);
    void* virt = taco_sys_mmap_noncache(phys, total_size);
    if (!virt) {
        LOG4CPLUS_ERROR(logger_, "OSD taco_sys_mmap_noncache failed");
        taco_sys_release_block(blk_id);
        return false;
    }

    memset(virt, 0, total_size);

    dma_mem_.blk_id = blk_id;
    dma_mem_.phys_addr = phys;
    dma_mem_.virt_addr = virt;
    dma_mem_.frame_size = frame_size;
    dma_mem_.total_size = total_size;

    LOG4CPLUS_INFO_FMT(logger_,
        "OSD DMA: blk_id=%u, phys=0x%llx, virt=%p, frame=%zu, total=%zu (%d buffers)",
        blk_id, (unsigned long long)phys, virt, frame_size, total_size, BUFFER_COUNT);
    return true;
}

void OsdOverlay::freeDmaMemory() {
    if (dma_mem_.virt_addr) {
        taco_sys_munmap(dma_mem_.virt_addr, dma_mem_.total_size);
        dma_mem_.virt_addr = nullptr;
    }
    if (dma_mem_.blk_id) {
        taco_sys_release_block(dma_mem_.blk_id);
        dma_mem_.blk_id = 0;
    }
    dma_mem_.phys_addr = 0;
    dma_mem_.frame_size = 0;
    dma_mem_.total_size = 0;
}

// ============================================================
// BufferPool 创建（4 个 DMA 页注入）
// ============================================================

bool OsdOverlay::createBufferPool() {
    allocator_ = std::make_unique<BufferAllocatorFacade>(
        BufferAllocatorFactory::AllocatorType::FRAMEBUFFER);
    if (!allocator_) {
        LOG4CPLUS_ERROR(logger_, "Failed to create BufferAllocatorFacade");
        return false;
    }

    pool_id_ = allocator_->allocatePoolWithBuffers(
        0, 0, "OsdOverlay_fb", "Display");
    if (pool_id_ == 0) {
        LOG4CPLUS_ERROR(logger_, "Failed to create OSD BufferPool");
        return false;
    }

    uint8_t* base = static_cast<uint8_t*>(dma_mem_.virt_addr);
    for (int i = 0; i < BUFFER_COUNT; i++) {
        void*    virt = base + dma_mem_.frame_size * i;
        uint64_t phys = dma_mem_.phys_addr + dma_mem_.frame_size * i;

        Buffer* buf = allocator_->injectExternalBufferToPool(
            pool_id_, virt, phys, dma_mem_.frame_size, QueueType::FREE);
        if (!buf) {
            LOG4CPLUS_ERROR_FMT(logger_, "Failed to inject OSD buffer #%d", i);
            pool_id_ = 0;
            allocator_.reset();
            return false;
        }
    }

    LOG4CPLUS_INFO_FMT(logger_, "OSD BufferPool created: %d buffers, %zu bytes each",
        BUFFER_COUNT, dma_mem_.frame_size);
    return true;
}

// ============================================================
// DSS overlay1 配置
// ============================================================

bool OsdOverlay::setupDssOverlay1() {
    write_sysfs(OVL1_PIX_FMT_PATH, "argb8888");
    write_sysfs(LCD_ALPHA_BLEND_PATH, "0");
    write_sysfs(LCD_TRANS_KEY_VALUE_PATH, "0");
    write_sysfs(LCD_TRANS_KEY_ENABLED_PATH, "1");

    struct tpsfb_dma_info dma_info;
    dma_info.ovl_idx = 1;
    dma_info.phys_addr = dma_mem_.phys_addr;

    if (ioctl(fb_fd_, FB_IOCTL_SET_DMA_INFO, &dma_info) < 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "OSD FB_IOCTL_SET_DMA_INFO failed: %s", strerror(errno));
        return false;
    }

    write_sysfs(OVL1_ENABLED_PATH, "1");

    LOG4CPLUS_INFO_FMT(logger_,
        "OSD DSS overlay1 configured: %dx%d ARGB8888, colorkey=0x000000, DMA=0x%llx",
        screen_width_, screen_height_, (unsigned long long)dma_mem_.phys_addr);
    return true;
}

// ============================================================
// FreeType 初始化
// ============================================================

bool OsdOverlay::initFreeType(const Config& config) {
    FT_Error err = FT_Init_FreeType(&ft_lib_);
    if (err) {
        LOG4CPLUS_ERROR_FMT(logger_, "FT_Init_FreeType failed: %d", err);
        return false;
    }

    err = FT_New_Face(ft_lib_, config.font_path.c_str(), 0, &ft_face_);
    if (err) {
        LOG4CPLUS_ERROR_FMT(logger_, "FT_New_Face failed: %d (font=%s)",
            err, config.font_path.c_str());
        FT_Done_FreeType(ft_lib_); ft_lib_ = nullptr;
        return false;
    }

    err = FT_Set_Pixel_Sizes(ft_face_, 0, config.font_size);
    if (err) {
        LOG4CPLUS_ERROR_FMT(logger_, "FT_Set_Pixel_Sizes failed: %d", err);
        FT_Done_Face(ft_face_); ft_face_ = nullptr;
        FT_Done_FreeType(ft_lib_); ft_lib_ = nullptr;
        return false;
    }

    LOG4CPLUS_INFO_FMT(logger_, "FreeType: %s, size=%d",
        config.font_path.c_str(), config.font_size);
    return true;
}

void OsdOverlay::cleanupFreeType() {
    if (ft_face_) { FT_Done_Face(ft_face_); ft_face_ = nullptr; }
    if (ft_lib_) { FT_Done_FreeType(ft_lib_); ft_lib_ = nullptr; }
}

// ============================================================
// 通道管理
// ============================================================

void OsdOverlay::registerChannel(int channel_id, int x, int y, int w, int h) {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    if (channel_id < 0) return;
    if (channel_id >= static_cast<int>(channels_.size())) {
        channels_.resize(channel_id + 1);
    }

    auto& ch = channels_[channel_id];
    ch.channel_id = channel_id;
    ch.x = x;
    ch.y = y;
    ch.w = w;
    ch.h = h;
    ch.active = true;
    ch.frame_count.store(0);
    ch.current_fps = 0.0;

    LOG4CPLUS_INFO_FMT(logger_, "OSD channel %d registered: (%d,%d,%d,%d)",
        channel_id, x, y, w, h);
}

void OsdOverlay::unregisterChannel(int channel_id) {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    if (channel_id >= 0 && channel_id < static_cast<int>(channels_.size())) {
        channels_[channel_id].active = false;
    }
}

void OsdOverlay::recordFrame(int channel_id) {
    if (channel_id >= 0 && channel_id < static_cast<int>(channels_.size())) {
        channels_[channel_id].frame_count.fetch_add(1, std::memory_order_relaxed);
    }
}

// ============================================================
// 定时器回调
// ============================================================

void OsdOverlay::onTimerTick() {
    int fps = config_.refresh_fps > 0 ? config_.refresh_fps : 1;

    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        for (auto& ch : channels_) {
            if (ch.active) {
                int count = ch.frame_count.exchange(0, std::memory_order_relaxed);
                ch.current_fps = static_cast<double>(count) * fps;
            }
        }
    }

    renderOsd();
}

// ============================================================
// OSD 渲染（acquireFree → 渲染 → submitFilled → acquireFilled → pan → releaseFilled旧页）
// ============================================================

void OsdOverlay::renderOsd() {
    auto pool = BufferPoolRegistry::getInstance().getPool(pool_id_).lock();
    if (!pool) return;

    Buffer* buf = pool->acquireFree(false, 0);
    if (!buf) return;

    uint32_t* pixels = static_cast<uint32_t*>(buf->getVirtualAddress());
    memset(pixels, 0, dma_mem_.frame_size);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);

    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_info);

    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        for (const auto& ch : channels_) {
            if (!ch.active) continue;

            char osd_text[128];
            snprintf(osd_text, sizeof(osd_text), "CH%d | %s | %.1ffps",
                     ch.channel_id + 1, time_str, ch.current_fps);

            drawText(ch.x + 4, ch.y + config_.font_size + 4, osd_text, 0xFFFFFFFF, pixels);
        }
    }

    pool->submitFilled(buf);

    Buffer* disp = pool->acquireFilled(false, 0);
    if (!disp) return;

    size_t offset = disp->getPhysicalAddress() - dma_mem_.phys_addr;
    int page_index = static_cast<int>(offset / dma_mem_.frame_size);

    struct fb_var_screeninfo var;
    if (ioctl(fb_fd_, FBIOGET_VSCREENINFO, &var) == 0) {
        var.yoffset = page_index * var.yres;
        ioctl(fb_fd_, FBIOPAN_DISPLAY, &var);
    }

    if (display_buf_) {
        pool->releaseFilled(display_buf_);
    }
    display_buf_ = disp;
}

// ============================================================
// 绘制函数
// ============================================================

void OsdOverlay::drawText(int x, int y, const std::string& text, uint32_t color, uint32_t* buf) {
    if (!ft_face_ || !buf) return;

    int pen_x = x;
    int pen_y = y;

    for (char c : text) {
        FT_Error err = FT_Load_Char(ft_face_, static_cast<FT_ULong>(c), FT_LOAD_RENDER);
        if (err) continue;

        FT_GlyphSlot glyph = ft_face_->glyph;
        drawCharGlyph(pen_x + glyph->bitmap_left, pen_y - glyph->bitmap_top, glyph, color, buf);
        pen_x += static_cast<int>(glyph->advance.x >> 6);
    }
}

void OsdOverlay::drawCharGlyph(int base_x, int base_y, FT_GlyphSlot glyph, uint32_t color, uint32_t* buf) {
    if (!buf) return;

    FT_Bitmap& bmp = glyph->bitmap;
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    for (unsigned int row = 0; row < bmp.rows; ++row) {
        int py = base_y + static_cast<int>(row);
        if (py < 0 || py >= screen_height_) continue;

        for (unsigned int col = 0; col < bmp.width; ++col) {
            int px = base_x + static_cast<int>(col);
            if (px < 0 || px >= screen_width_) continue;

            uint8_t alpha = bmp.buffer[row * bmp.pitch + col];
            if (alpha == 0) continue;

            buf[py * screen_width_ + px] =
                (static_cast<uint32_t>(alpha) << 24) |
                (static_cast<uint32_t>(r) << 16) |
                (static_cast<uint32_t>(g) << 8) |
                static_cast<uint32_t>(b);
        }
    }
}
