#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <cstring>
#include <cerrno>

#include "vendor/taco/display/TacoProDisplayContext.hpp"
#include "bufferpool/buffer/RawBuffer.hpp"
#include "common/ImageMeta.hpp"
#include "vendor/taco/display/TacoProOsdOverlay.hpp"
#include "vendor/taco/memory/TacoMemoryProvider.hpp"
#include "vendor/taco/stitcher/TacoProStitcherDriver.hpp"
#include "common/Logger.hpp"
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

// === 单例静态成员 ===
std::mutex TacoProDisplayContext::s_acquire_mutex_;
std::weak_ptr<TacoProDisplayContext> TacoProDisplayContext::s_instance_;

// ============================================================
// 单例获取
// ============================================================

std::shared_ptr<TacoProDisplayContext> TacoProDisplayContext::acquire(const TacoProDisplayExtension& config) {
    std::lock_guard<std::mutex> lock(s_acquire_mutex_);

    auto existing = s_instance_.lock();
    if (existing) {
        return existing;
    }

    auto ctx = std::shared_ptr<TacoProDisplayContext>(new TacoProDisplayContext(config));
    if (!ctx->open()) {
        return nullptr;
    }
    s_instance_ = ctx;
    return ctx;
}

// ============================================================
// 构造 / 析构
// ============================================================

TacoProDisplayContext::TacoProDisplayContext(const TacoProDisplayExtension& config)
    : config_(config)
    , fd_(-1)
    , fb_index_(0)
    , screen_width_(config.screen_width)
    , screen_height_(config.screen_height)
    , bits_per_pixel_(32)
    , buffer_size_(0)
    , buffer_count_(4)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Display.SharedContext")))
{
}

TacoProDisplayContext::~TacoProDisplayContext() {
    close();
}

// ============================================================
// open / close（对齐 Worker 生命周期命名）
// ============================================================

bool TacoProDisplayContext::open() {
    {
        FILE* f = fopen("/sys/devices/platform/soc/soc:dss@c9200000/dss-overlay1/enabled", "w");
        if (f) { fprintf(f, "0"); fclose(f); }
    }

    if (!openDevice()) {
        return false;
    }

    if (!createBufferPool()) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    auto pool = getBufferPool();
    if (!pool) {
        fb_pool_id_ = 0;
        builder_.reset();
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // 分配专用模板帧（独立 TACO 内存，不进 BufferPool）
    template_blk_id_ = taco_sys_get_block(
        TACO_INVALID_POOLID, buffer_size_, "template_frame");
    if (template_blk_id_ == 0) {
        LOG4CPLUS_ERROR(logger_, "Failed to allocate TACO block for template frame");
        fb_pool_id_ = 0;
        builder_.reset();
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    uint64_t tmpl_phys = taco_sys_handle2_phys_addr(template_blk_id_);
    void* tmpl_virt = taco_sys_mmap_noncache(
        tmpl_phys, static_cast<uint32_t>(buffer_size_));
    if (!tmpl_virt) {
        LOG4CPLUS_ERROR(logger_, "Failed to mmap template frame");
        taco_sys_release_block(template_blk_id_);
        template_blk_id_ = 0;
        fb_pool_id_ = 0;
        builder_.reset();
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    {
        size_t y_plane_size  = static_cast<size_t>(screen_width_) * screen_height_;
        size_t uv_plane_size = y_plane_size / 2;
        uint8_t* p = static_cast<uint8_t*>(tmpl_virt);
        memset(p, 0, y_plane_size);
        memset(p + y_plane_size, 128, uv_plane_size);
    }
    auto template_buf = std::make_unique<RawBuffer>(
        template_blk_id_, tmpl_virt, tmpl_phys, buffer_size_,
        Buffer::Ownership::EXTERNAL);

    LOG4CPLUS_INFO_FMT(logger_,
        "Template frame allocated: blk_id=%u, phys=0x%llx, size=%zu (NV12 Y=0 UV=128)",
        template_blk_id_, (unsigned long long)tmpl_phys, buffer_size_);

    // Create hardware stitcher driver
    auto driver = std::make_shared<TacoProStitcherDriver>();

    // Create stitcher service config
    FrameStitcherConfig stitch_config;
    stitch_config.screen_width = screen_width_;
    stitch_config.screen_height = screen_height_;
    stitch_config.bits_per_pixel = bits_per_pixel_;
    stitch_config.target_fps = config_.target_fps;
    stitch_config.view_type = config_.view_type;
    stitch_config.slot_assignment = config_.slot_assignment;
    stitch_config.main_sidebar_ratio = config_.main_sidebar_ratio;

    stitcher_ = std::make_shared<FrameStitcherService>(
        stitch_config, driver, pool, std::move(template_buf));

    if (!stitcher_->start()) {
        LOG4CPLUS_ERROR(logger_, "Failed to start FrameStitcherService");
        stitcher_.reset();
        taco_sys_munmap(tmpl_virt, static_cast<uint32_t>(buffer_size_));
        taco_sys_release_block(template_blk_id_);
        template_blk_id_ = 0;
        fb_pool_id_ = 0;
        builder_.reset();
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // 注册 Display 订阅者（DMA 送显逻辑）
    stitcher_->subscribe([this](const StitchedFrame& frame) {
        struct tpsfb_dma_info dma_info;
        dma_info.ovl_idx = 0;
        dma_info.phys_addr = frame.buffer->getPhysicalAddress();
        if (ioctl(fd_, FB_IOCTL_SET_DMA_INFO, &dma_info) < 0) {
            LOG4CPLUS_WARN_FMT(logger_,
                "Display: FB_IOCTL_SET_DMA_INFO failed: %s", strerror(errno));
            return;
        }

        struct fb_var_screeninfo var_info;
        if (ioctl(fd_, FBIOGET_VSCREENINFO, &var_info) < 0) {
            LOG4CPLUS_WARN_FMT(logger_,
                "Display: FBIOGET_VSCREENINFO failed: %s", strerror(errno));
            return;
        }

        var_info.yoffset = 0;
        if (ioctl(fd_, FBIOPAN_DISPLAY, &var_info) < 0) {
            LOG4CPLUS_WARN_FMT(logger_,
                "Display: FBIOPAN_DISPLAY failed: %s", strerror(errno));
            return;
        }

        int zero = 0;
        ioctl(fd_, FBIO_WAITFORVSYNC, &zero);
    });

    if (config_.osd_enable) {
        osd_ = std::make_unique<TacoProOsdOverlay>(screen_width_, screen_height_, 64);
        TacoProOsdOverlay::Config osd_cfg;
        osd_cfg.refresh_fps = config_.osd_fps;
        osd_cfg.font_path   = config_.osd_font_path;
        osd_cfg.font_size   = config_.osd_font_size;

        if (!osd_->init(osd_cfg)) {
            LOG4CPLUS_WARN(logger_, "OSD initialization failed, continuing without OSD");
            osd_.reset();
        }
    }

    LOG4CPLUS_INFO_FMT(logger_,
        "TacoProDisplayContext opened: %dx%d, %dbpp, %d buffers, %dfps, view=%s, osd=%s",
        screen_width_, screen_height_, bits_per_pixel_, buffer_count_, config_.target_fps,
        (stitcher_->getViewType() == ViewType::GRID ? "grid" : "main_sidebar"),
        osd_ ? "on" : "off");

    LOG4CPLUS_INFO(logger_, "View layout:\n" << stitcher_->getViewDiagram());

    return true;
}

void TacoProDisplayContext::close() {
    if (osd_) {
        osd_->shutdown();
        osd_.reset();
    }

    // Stop stitcher (stops render thread + tick timer + releases displayed_buf_)
    std::unique_ptr<Buffer> template_buf;
    if (stitcher_) {
        stitcher_->stop();
        template_buf = stitcher_->takeTemplateBuf();
        stitcher_.reset();
    }

    // Shutdown pool (unblocks any waiting acquireFree/acquireFilled)
    auto pool = getBufferPool();
    if (pool) {
        pool->shutdown();
    }

    // 释放模板帧（独立 TACO 内存）
    if (template_buf) {
        void* virt = template_buf->getVirtualAddress();
        if (virt) {
            taco_sys_munmap(virt, static_cast<uint32_t>(buffer_size_));
        }
        template_buf.reset();
    }
    if (template_blk_id_ != 0) {
        taco_sys_release_block(template_blk_id_);
        template_blk_id_ = 0;
    }

    // destroyPool 内部自动 taco_sys_munmap + taco_sys_release_block
    fb_pool_id_ = 0;
    builder_.reset();

    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }

    LOG4CPLUS_INFO(logger_, "TacoProDisplayContext closed");
}

// ============================================================
// Framebuffer 设备（对齐 Worker::open 中的设备初始化）
// ============================================================

bool TacoProDisplayContext::openDevice() {
    FILE* fp = fopen(kProcFb, "r");
    if (!fp) {
        LOG4CPLUS_ERROR_FMT(logger_, "Cannot open %s: %s", kProcFb, strerror(errno));
        return false;
    }

    const char* device_node = nullptr;
    char line[256];
    int fb_num;
    char fb_name[32];

    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "%d %s", &fb_num, fb_name) == 2) {
            if (strcmp(fb_name, kTpsFb0) == 0) {
                fb_index_ = 0;
                if (fb_num >= 0 && fb_num < static_cast<int>(kDevFbPaths.size())) {
                    device_node = kDevFbPaths[fb_num];
                }
                break;
            }
        }
    }
    fclose(fp);

    if (!device_node) {
        LOG4CPLUS_WARN(logger_, "tpsfb0 not found in /proc/fb (display hardware may not be present on this host)");
        return false;
    }

    fd_ = ::open(device_node, O_RDWR);
    if (fd_ < 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "Cannot open %s: %s", device_node, strerror(errno));
        return false;
    }

    // 视频层使用 NV12 格式（设备树默认 ARGB8888，需运行时切换）
    {
        static const char* OVL0_PIX_FMT_PATH =
            "/sys/devices/platform/soc/soc:dss@c9200000/dss-overlay0/pixel_fmt";
        FILE* f = fopen(OVL0_PIX_FMT_PATH, "w");
        if (f) { fprintf(f, "nv12"); fclose(f); }
    }

    struct fb_var_screeninfo var_info;
    if (ioctl(fd_, FBIOGET_VSCREENINFO, &var_info) < 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "FBIOGET_VSCREENINFO failed: %s", strerror(errno));
        return false;
    }

    screen_width_   = var_info.xres;
    screen_height_  = var_info.yres;
    bits_per_pixel_ = 12;
    buffer_count_ = 8;

    size_t total_bits = static_cast<size_t>(screen_width_) * screen_height_ * bits_per_pixel_;
    buffer_size_ = (total_bits + 7) / 8;

    LOG4CPLUS_INFO_FMT(logger_, "FB device: %s, %dx%d, %dbpp, %d buffers, buf_size=%zu",
        device_node, screen_width_, screen_height_, bits_per_pixel_, buffer_count_, buffer_size_);

    return true;
}

// ============================================================
// BufferPool 创建（TACO 分配由 FramebufferAllocator 内部完成）
// ============================================================

bool TacoProDisplayContext::createBufferPool() {
    // 确保 TACO 内存提供者已注册（显式调用，避免链接器丢弃目标文件）
    register_taco_memory_provider();

    builder_ = BufferPoolBuilderFactory::createWithProvider(
        BufferPoolBuilderFactory::AllocatorType::CONTINUOUS_PHYSICAL,
        "taco");

    fb_pool_id_ = builder_->allocatePoolWithBuffers(
        buffer_count_, buffer_size_, "TacoProDisplayContext_fb", "Display");
    if (fb_pool_id_ == 0) {
        LOG4CPLUS_ERROR(logger_, "Failed to create BufferPool");
        builder_.reset();
        return false;
    }

    return true;
}

std::shared_ptr<BufferPool> TacoProDisplayContext::getBufferPool() {
    if (fb_pool_id_ == 0) return nullptr;
    return ComponentTopology::getInstance().getPool(fb_pool_id_).lock();
}

// ============================================================
// 通道管理（委托给 stitcher_ + OSD）
// ============================================================

int TacoProDisplayContext::registerChannel() {
    int id = stitcher_->registerChannel();
    if (id >= 0 && osd_) {
        auto layout = stitcher_->getSlotLayout(id);
        osd_->registerChannel(id, layout.x, layout.y, layout.w, layout.h);
    }
    return id;
}

int TacoProDisplayContext::registerChannel(const ChannelLayout& layout) {
    int id = stitcher_->registerChannel(layout);
    if (id >= 0 && osd_) {
        osd_->registerChannel(id, layout.x, layout.y, layout.w, layout.h);
    }
    return id;
}

void TacoProDisplayContext::unregisterChannel(int channel_id) {
    stitcher_->unregisterChannel(channel_id);
    if (osd_) {
        osd_->unregisterChannel(channel_id);
    }
}

// ============================================================
// 通道写入（委托给 stitcher_ + OSD）
// ============================================================

bool TacoProDisplayContext::channelWrite(int channel_id, Buffer* decoded) {
    bool result = stitcher_->channelWrite(channel_id, decoded);
    if (result && osd_) {
        osd_->recordFrame(channel_id);
    }
    return result;
}

// ============================================================
// 视图查询（委托给 stitcher_）
// ============================================================

ViewType TacoProDisplayContext::getViewType() const {
    return stitcher_->getViewType();
}

int TacoProDisplayContext::getSlotCount() const {
    return stitcher_->getSlotCount();
}

const ChannelLayout& TacoProDisplayContext::getSlotLayout(int slot_index) const {
    return stitcher_->getSlotLayout(slot_index);
}

std::string TacoProDisplayContext::getViewDiagram() const {
    return stitcher_->getViewDiagram();
}

