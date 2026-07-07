#include "vendor/taco/display/TacoProDisplayDevice.hpp"
#include "bufferpool/buffer/RawBuffer.hpp"
#include "common/ImageMeta.hpp"
#include "vendor/taco/display/TacoProOsdOverlay.hpp"
#include "vendor/taco/memory/TacoMemoryProvider.hpp"
#include "vendor/taco/stitcher/TacoProStitcherDriver.hpp"
#include "common/Logger.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <cstring>
#include <cerrno>
#include <array>

extern "C" {
#include "ta_sys_api.h"
}

namespace {
    constexpr const char* kProcFb = "/proc/fb";
    constexpr const char* kTpsFb0 = "tpsfb0";

    constexpr std::array<const char*, 3> kDevFbPaths = {{
        "/dev/fb0",
        "/dev/fb1",
        "/dev/fb2",
    }};
}

struct tpsfb_dma_info {
    uint32_t ovl_idx;
    uint64_t phys_addr;
};
#define FB_IOCTL_SET_DMA_INFO _IOW('F', 7, struct tpsfb_dma_info)

// === Static Members ===
std::mutex TacoProDisplayDevice::s_state_mutex;
std::weak_ptr<TacoProDisplayDevice::SharedState> TacoProDisplayDevice::s_shared_state;

// ============================================================
// TacoProDisplayDevice::SharedState Implementation
// ============================================================

TacoProDisplayDevice::SharedState::SharedState(const TacoProDisplayExtension& cfg)
    : config(cfg)
    , logger(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Display.SharedState")))
{
}

TacoProDisplayDevice::SharedState::~SharedState() {
    close();
}

bool TacoProDisplayDevice::SharedState::open() {
    {
        FILE* f = fopen("/sys/devices/platform/soc/soc:dss@c9200000/dss-overlay1/enabled", "w");
        if (f) { fprintf(f, "0"); fclose(f); }
    }

    if (!openDevice()) {
        return false;
    }

    if (!createBufferPool()) {
        ::close(fd);
        fd = -1;
        return false;
    }

    auto pool = getBufferPool();
    if (!pool) {
        fb_pool_id = 0;
        builder.reset();
        ::close(fd);
        fd = -1;
        return false;
    }

    // 分配专用模板帧（独立 TACO 内存，不进 BufferPool）
    template_blk_id = taco_sys_get_block(
        TACO_INVALID_POOLID, buffer_size, "template_frame");
    if (template_blk_id == 0) {
        LOG4CPLUS_ERROR(logger, "Failed to allocate TACO block for template frame");
        fb_pool_id = 0;
        builder.reset();
        ::close(fd);
        fd = -1;
        return false;
    }

    uint64_t tmpl_phys = taco_sys_handle2_phys_addr(template_blk_id);
    void* tmpl_virt = taco_sys_mmap_noncache(
        tmpl_phys, static_cast<uint32_t>(buffer_size));
    if (!tmpl_virt) {
        LOG4CPLUS_ERROR(logger, "Failed to mmap template frame");
        taco_sys_release_block(template_blk_id);
        template_blk_id = 0;
        fb_pool_id = 0;
        builder.reset();
        ::close(fd);
        fd = -1;
        return false;
    }

    {
        size_t y_plane_size  = static_cast<size_t>(screen_width) * screen_height;
        size_t uv_plane_size = y_plane_size / 2;
        uint8_t* p = static_cast<uint8_t*>(tmpl_virt);
        memset(p, 0, y_plane_size);
        memset(p + y_plane_size, 128, uv_plane_size);
    }
    auto template_buf = std::make_unique<RawBuffer>(
        template_blk_id, tmpl_virt, tmpl_phys, buffer_size,
        Buffer::Ownership::EXTERNAL);

    LOG4CPLUS_INFO_FMT(logger,
        "Template frame allocated: blk_id=%u, phys=0x%llx, size=%zu (NV12 Y=0 UV=128)",
        template_blk_id, (unsigned long long)tmpl_phys, buffer_size);

    // Create hardware stitcher driver
    auto drv = std::make_shared<TacoProStitcherDriver>();

    // Create stitcher service config
    FrameStitcherConfig stitch_config;
    stitch_config.screen_width = screen_width;
    stitch_config.screen_height = screen_height;
    stitch_config.bits_per_pixel = bits_per_pixel;
    stitch_config.target_fps = config.target_fps;
    stitch_config.view_type = config.view_type;
    stitch_config.slot_assignment = config.slot_assignment;
    stitch_config.main_sidebar_ratio = config.main_sidebar_ratio;

    stitcher = std::make_shared<FrameStitcherService>(
        stitch_config, drv, pool, std::move(template_buf));

    if (!stitcher->start()) {
        LOG4CPLUS_ERROR(logger, "Failed to start FrameStitcherService");
        stitcher.reset();
        taco_sys_munmap(tmpl_virt, static_cast<uint32_t>(buffer_size));
        taco_sys_release_block(template_blk_id);
        template_blk_id = 0;
        fb_pool_id = 0;
        builder.reset();
        ::close(fd);
        fd = -1;
        return false;
    }

    // 注册 Display 订阅者（DMA 送显逻辑）
    stitcher->subscribe([this](const StitchedFrame& frame) {
        struct tpsfb_dma_info dma_info;
        dma_info.ovl_idx = 0;
        dma_info.phys_addr = frame.buffer->getPhysicalAddress();
        if (ioctl(fd, FB_IOCTL_SET_DMA_INFO, &dma_info) < 0) {
            LOG4CPLUS_WARN_FMT(logger,
                "Display: FB_IOCTL_SET_DMA_INFO failed: %s", strerror(errno));
            return;
        }

        struct fb_var_screeninfo var_info;
        if (ioctl(fd, FBIOGET_VSCREENINFO, &var_info) < 0) {
            LOG4CPLUS_WARN_FMT(logger,
                "Display: FBIOGET_VSCREENINFO failed: %s", strerror(errno));
            return;
        }

        var_info.yoffset = 0;
        if (ioctl(fd, FBIOPAN_DISPLAY, &var_info) < 0) {
            LOG4CPLUS_WARN_FMT(logger,
                "Display: FBIOPAN_DISPLAY failed: %s", strerror(errno));
            return;
        }

        int zero = 0;
        ioctl(fd, FBIO_WAITFORVSYNC, &zero);
    });

    if (config.osd_enable) {
        osd = std::make_unique<TacoProOsdOverlay>(screen_width, screen_height, 64);
        TacoProOsdOverlay::Config osd_cfg;
        osd_cfg.refresh_fps = config.osd_fps;
        osd_cfg.font_path   = config.osd_font_path;
        osd_cfg.font_size   = config.osd_font_size;

        if (!osd->init(osd_cfg)) {
            LOG4CPLUS_WARN(logger, "OSD initialization failed, continuing without OSD");
            osd.reset();
        }
    }

    LOG4CPLUS_INFO_FMT(logger,
        "SharedState opened: %dx%d, %dbpp, %d buffers, %dfps, view=%s, osd=%s",
        screen_width, screen_height, bits_per_pixel, buffer_count, config.target_fps,
        (stitcher->getViewType() == ViewType::GRID ? "grid" : "main_sidebar"),
        osd ? "on" : "off");

    LOG4CPLUS_INFO(logger, "View layout:\n" << stitcher->getViewDiagram());

    return true;
}

void TacoProDisplayDevice::SharedState::close() {
    if (osd) {
        osd->shutdown();
        osd.reset();
    }

    std::unique_ptr<Buffer> template_buf;
    if (stitcher) {
        stitcher->stop();
        template_buf = stitcher->takeTemplateBuf();
        stitcher.reset();
    }

    auto pool = getBufferPool();
    if (pool) {
        pool->shutdown();
    }

    if (template_buf) {
        void* virt = template_buf->getVirtualAddress();
        if (virt) {
            taco_sys_munmap(virt, static_cast<uint32_t>(buffer_size));
        }
        template_buf.reset();
    }
    if (template_blk_id != 0) {
        taco_sys_release_block(template_blk_id);
        template_blk_id = 0;
    }

    fb_pool_id = 0;
    builder.reset();

    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }

    LOG4CPLUS_INFO(logger, "SharedState closed");
}

bool TacoProDisplayDevice::SharedState::openDevice() {
    FILE* fp = fopen(kProcFb, "r");
    if (!fp) {
        LOG4CPLUS_ERROR_FMT(logger, "Cannot open %s: %s", kProcFb, strerror(errno));
        return false;
    }

    const char* device_node = nullptr;
    char line[256];
    int fb_num;
    char fb_name[32];

    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "%d %s", &fb_num, fb_name) == 2) {
            if (strcmp(fb_name, kTpsFb0) == 0) {
                fb_index = 0;
                if (fb_num >= 0 && fb_num < static_cast<int>(kDevFbPaths.size())) {
                    device_node = kDevFbPaths[fb_num];
                }
                break;
            }
        }
    }
    fclose(fp);

    if (!device_node) {
        LOG4CPLUS_WARN(logger, "tpsfb0 not found in /proc/fb");
        return false;
    }

    fd = ::open(device_node, O_RDWR);
    if (fd < 0) {
        LOG4CPLUS_ERROR_FMT(logger, "Cannot open %s: %s", device_node, strerror(errno));
        return false;
    }

    {
        static const char* OVL0_PIX_FMT_PATH =
            "/sys/devices/platform/soc/soc:dss@c9200000/dss-overlay0/pixel_fmt";
        FILE* f = fopen(OVL0_PIX_FMT_PATH, "w");
        if (f) { fprintf(f, "nv12"); fclose(f); }
    }

    struct fb_var_screeninfo var_info;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var_info) < 0) {
        LOG4CPLUS_ERROR_FMT(logger, "FBIOGET_VSCREENINFO failed: %s", strerror(errno));
        return false;
    }

    screen_width   = var_info.xres;
    screen_height  = var_info.yres;
    bits_per_pixel = 12;
    buffer_count = 8;

    size_t total_bits = static_cast<size_t>(screen_width) * screen_height * bits_per_pixel;
    buffer_size = (total_bits + 7) / 8;

    LOG4CPLUS_INFO_FMT(logger, "FB device: %s, %dx%d, %dbpp, %d buffers, buf_size=%zu",
        device_node, screen_width, screen_height, bits_per_pixel, buffer_count, buffer_size);

    return true;
}

bool TacoProDisplayDevice::SharedState::createBufferPool() {
    register_taco_memory_provider();

    builder = BufferPoolBuilderFactory::createWithProvider(
        BufferPoolBuilderFactory::AllocatorType::CONTINUOUS_PHYSICAL,
        "taco");

    fb_pool_id = builder->allocatePoolWithBuffers(
        buffer_count, buffer_size, "TacoProDisplayDevice_fb", "Display");
    if (fb_pool_id == 0) {
        LOG4CPLUS_ERROR(logger, "Failed to create BufferPool");
        builder.reset();
        return false;
    }

    return true;
}

std::shared_ptr<BufferPool> TacoProDisplayDevice::SharedState::getBufferPool() {
    if (fb_pool_id == 0) return nullptr;
    return ComponentTopology::getInstance().getPool(fb_pool_id).lock();
}

int TacoProDisplayDevice::SharedState::registerChannel() {
    int id = stitcher->registerChannel();
    if (id >= 0 && osd) {
        auto layout = stitcher->getSlotLayout(id);
        osd->registerChannel(id, layout.x, layout.y, layout.w, layout.h);
    }
    return id;
}

int TacoProDisplayDevice::SharedState::registerChannel(const ChannelLayout& layout) {
    int id = stitcher->registerChannel(layout);
    if (id >= 0 && osd) {
        osd->registerChannel(id, layout.x, layout.y, layout.w, layout.h);
    }
    return id;
}

void TacoProDisplayDevice::SharedState::unregisterChannel(int channel_id) {
    stitcher->unregisterChannel(channel_id);
    if (osd) {
        osd->unregisterChannel(channel_id);
    }
}

bool TacoProDisplayDevice::SharedState::channelWrite(int channel_id, Buffer* decoded) {
    bool result = stitcher->channelWrite(channel_id, decoded);
    if (result && osd) {
        osd->recordFrame(channel_id);
    }
    return result;
}

// ============================================================
// TacoProDisplayDevice Implementation
// ============================================================

TacoProDisplayDevice::TacoProDisplayDevice(const TacoProDisplayExtension& config)
    : config_(config)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Display.TacoPro")))
{
}

TacoProDisplayDevice::~TacoProDisplayDevice() {
    cleanup();
}

const char* TacoProDisplayDevice::findDeviceNode(int /*device_index*/) {
    return "tacopro-display";
}

bool TacoProDisplayDevice::initialize(int /*device_index*/) {
    if (initialized_) {
        return true;
    }

    std::lock_guard<std::mutex> lock(s_state_mutex);
    state_ = s_shared_state.lock();
    if (!state_) {
        state_ = std::make_shared<SharedState>(config_);
        if (!state_->open()) {
            state_.reset();
            LOG4CPLUS_WARN(logger_, "Failed to open SharedState (display hardware unavailable)");
            return false;
        }
        s_shared_state = state_;
    }

    channel_id_ = state_->registerChannel();
    if (channel_id_ < 0) {
        LOG4CPLUS_ERROR(logger_, "Failed to register channel");
        state_.reset();
        return false;
    }

    initialized_ = true;
    LOG4CPLUS_INFO_FMT(logger_,
        "TacoProDisplayDevice initialized: channel=%d", channel_id_);

    return true;
}

void TacoProDisplayDevice::cleanup() {
    if (!initialized_) {
        return;
    }

    if (state_) {
        if (channel_id_ >= 0) {
            state_->unregisterChannel(channel_id_);
            channel_id_ = -1;
        }
        state_.reset();
    }

    initialized_ = false;
    LOG4CPLUS_DEBUG(logger_, "TacoProDisplayDevice cleaned up");
}

bool TacoProDisplayDevice::displayBuffer(Buffer* buffer) {
    if (!initialized_ || !state_ || channel_id_ < 0) {
        last_display_failed_ = true;
        return false;
    }

    bool success = state_->channelWrite(channel_id_, buffer);
    last_display_failed_ = !success;
    return success;
}

bool TacoProDisplayDevice::displayBuffer(BufferPool* /*pool*/, int /*buffer_index*/) {
    return false;
}

bool TacoProDisplayDevice::waitVerticalSync() {
    return true;
}

int TacoProDisplayDevice::getWidth() const {
    return state_ ? state_->screen_width : config_.screen_width;
}

int TacoProDisplayDevice::getHeight() const {
    return state_ ? state_->screen_height : config_.screen_height;
}

int TacoProDisplayDevice::getBytesPerPixel() const {
    int bpp = state_ ? state_->bits_per_pixel : config_.bits_per_pixel;
    return (bpp + 7) / 8;
}

int TacoProDisplayDevice::getBitsPerPixel() const {
    return state_ ? state_->bits_per_pixel : config_.bits_per_pixel;
}

int TacoProDisplayDevice::getBufferCount() const {
    return config_.frame_pool_size;
}

size_t TacoProDisplayDevice::getBufferSize() const {
    int w = getWidth();
    int h = getHeight();
    int bpp = getBitsPerPixel();
    return static_cast<size_t>(w) * h * bpp / 8;
}

int TacoProDisplayDevice::getCurrentDisplayBuffer() const {
    return 0;
}

std::shared_ptr<FrameStitcherService> TacoProDisplayDevice::getStitcher() {
    std::lock_guard<std::mutex> lock(s_state_mutex);
    auto state = s_shared_state.lock();
    return state ? state->stitcher : nullptr;
}
