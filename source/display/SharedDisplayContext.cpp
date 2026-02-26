#include "display/SharedDisplayContext.hpp"
#include "display/OsdOverlay.hpp"
#include "common/Logger.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <linux/fb.h>
#include <cstring>
#include <cerrno>
#include <cmath>

#define PROC_FB "/proc/fb"
#define TPS_FB0 "tpsfb0"
#define TPS_FB1 "tpsfb1"
#define DEV_FB0 "/dev/fb0"
#define DEV_FB1 "/dev/fb1"
#define DEV_FB2 "/dev/fb2"

struct tpsfb_dma_info {
    uint32_t ovl_idx;
    uint64_t phys_addr;
};
#define FB_IOCTL_SET_DMA_INFO _IOW('F', 7, struct tpsfb_dma_info)

// === 单例静态成员 ===
std::mutex SharedDisplayContext::s_acquire_mutex_;
std::weak_ptr<SharedDisplayContext> SharedDisplayContext::s_instance_;

// ============================================================
// 单例获取
// ============================================================

std::shared_ptr<SharedDisplayContext> SharedDisplayContext::acquire(const TacoVOConfig& config) {
    std::lock_guard<std::mutex> lock(s_acquire_mutex_);

    auto existing = s_instance_.lock();
    if (existing) {
        return existing;
    }

    auto ctx = std::shared_ptr<SharedDisplayContext>(new SharedDisplayContext(config));
    if (!ctx->init()) {
        return nullptr;
    }
    s_instance_ = ctx;
    return ctx;
}

// ============================================================
// 构造 / 析构
// ============================================================

SharedDisplayContext::SharedDisplayContext(const TacoVOConfig& config)
    : config_(config)
    , fd_(-1)
    , fb_index_(0)
    , screen_width_(config.screen_width)
    , screen_height_(config.screen_height)
    , bits_per_pixel_(32)
    , buffer_size_(0)
    , buffer_count_(4)
    , render_buf_(nullptr)
    , display_buf_(nullptr)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Display.SharedContext")))
{
    int max_ch = config_.max_channels > 0 ? config_.max_channels : 64;
    channels_.reserve(max_ch);
}

SharedDisplayContext::~SharedDisplayContext() {
    shutdown();
}

// ============================================================
// 初始化
// ============================================================

bool SharedDisplayContext::init() {
    {
        FILE* f = fopen("/sys/devices/platform/soc/soc:dss@c9200000/dss-overlay1/enabled", "w");
        if (f) { fprintf(f, "0"); fclose(f); }
    }

    if (!openFramebufferDevice()) {
        return false;
    }

    if (!allocateDmaMemory()) {
        close(fd_);
        fd_ = -1;
        return false;
    }

    if (!setupDssForDma()) {
        freeDmaMemory();
        close(fd_);
        fd_ = -1;
        return false;
    }

    if (!createFramebufferPool()) {
        freeDmaMemory();
        close(fd_);
        fd_ = -1;
        return false;
    }

    // 从 FREE 队列获取第一个 buffer 作为初始渲染目标
    auto pool = getPool();
    if (!pool) {
        freeDmaMemory();
        close(fd_);
        fd_ = -1;
        return false;
    }

    render_buf_ = pool->acquireFree(false, 0);
    if (!render_buf_) {
        LOG4CPLUS_ERROR(logger_, "Failed to acquire initial render buffer");
        freeDmaMemory();
        close(fd_);
        fd_ = -1;
        return false;
    }

    // 清零初始渲染 buffer
    memset(render_buf_->getVirtualAddress(), 0, buffer_size_);

    if (!startThreads()) {
        pool->releaseFree(render_buf_);
        render_buf_ = nullptr;
        freeDmaMemory();
        close(fd_);
        fd_ = -1;
        return false;
    }

    // OSD 叠加初始化（可选）
    if (config_.osd_enable) {
        osd_ = std::make_unique<OsdOverlay>(screen_width_, screen_height_, config_.max_channels);
        OsdOverlay::Config osd_cfg;
        osd_cfg.refresh_fps = config_.osd_fps;
        osd_cfg.font_path   = config_.osd_font_path;
        osd_cfg.font_size   = config_.osd_font_size;

        if (!osd_->init(osd_cfg)) {
            LOG4CPLUS_WARN(logger_, "OSD initialization failed, continuing without OSD");
            osd_.reset();
        }
    }

    LOG4CPLUS_INFO_FMT(logger_,
        "SharedDisplayContext initialized: %dx%d, %dbpp, %d buffers, %dfps, osd=%s",
        screen_width_, screen_height_, bits_per_pixel_, buffer_count_, config_.target_fps,
        osd_ ? "on" : "off");

    return true;
}

void SharedDisplayContext::shutdown() {
    if (osd_) {
        osd_->shutdown();
        osd_.reset();
    }

    stopThreads();

    auto pool = getPool();

    // 归还 render_buf_
    if (render_buf_ && pool) {
        pool->releaseFree(render_buf_);
        render_buf_ = nullptr;
    }

    // 归还 display_buf_
    if (display_buf_ && pool) {
        pool->releaseFilled(display_buf_);
        display_buf_ = nullptr;
    }

    // 销毁 BufferPool
    fb_pool_id_ = 0;
    allocator_.reset();

    freeDmaMemory();

    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }

    LOG4CPLUS_INFO(logger_, "SharedDisplayContext shutdown complete");
}

// ============================================================
// Framebuffer 设备
// ============================================================

bool SharedDisplayContext::openFramebufferDevice() {
    FILE* fp = fopen(PROC_FB, "r");
    if (!fp) {
        LOG4CPLUS_ERROR_FMT(logger_, "Cannot open %s: %s", PROC_FB, strerror(errno));
        return false;
    }

    const char* device_node = nullptr;
    char line[256];
    int fb_num;
    char fb_name[32];

    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "%d %s", &fb_num, fb_name) == 2) {
            if (strcmp(fb_name, TPS_FB0) == 0) {
                fb_index_ = 0;
                if (fb_num == 0) device_node = DEV_FB0;
                else if (fb_num == 1) device_node = DEV_FB1;
                else if (fb_num == 2) device_node = DEV_FB2;
                break;
            }
        }
    }
    fclose(fp);

    if (!device_node) {
        LOG4CPLUS_ERROR(logger_, "tpsfb0 not found in /proc/fb");
        return false;
    }

    fd_ = open(device_node, O_RDWR);
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
    buffer_count_   = var_info.yres_virtual / var_info.yres;
    if (buffer_count_ < 2) buffer_count_ = 4;

    size_t total_bits = static_cast<size_t>(screen_width_) * screen_height_ * bits_per_pixel_;
    buffer_size_ = (total_bits + 7) / 8;

    LOG4CPLUS_INFO_FMT(logger_, "FB device: %s, %dx%d, %dbpp, %d buffers, buf_size=%zu",
        device_node, screen_width_, screen_height_, bits_per_pixel_, buffer_count_, buffer_size_);

    return true;
}

// ============================================================
// DMA 内存分配
// ============================================================

bool SharedDisplayContext::allocateDmaMemory() {
    dma_mem_.total_size = buffer_size_ * buffer_count_;

    dma_mem_.blk_id = taco_sys_get_block(
        TACO_INVALID_POOLID,
        dma_mem_.total_size,
        "shared_display_ctx");

    if (dma_mem_.blk_id == 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "taco_sys_get_block failed (size=%zu)", dma_mem_.total_size);
        return false;
    }

    dma_mem_.phys_addr = taco_sys_handle2_phys_addr(dma_mem_.blk_id);
    dma_mem_.virt_addr = taco_sys_mmap_noncache(
        dma_mem_.phys_addr, static_cast<uint32_t>(dma_mem_.total_size));

    if (!dma_mem_.virt_addr) {
        LOG4CPLUS_ERROR(logger_, "taco_sys_mmap_noncache failed");
        taco_sys_release_block(dma_mem_.blk_id);
        dma_mem_.blk_id = 0;
        return false;
    }

    memset(dma_mem_.virt_addr, 0, dma_mem_.total_size);

    // 初始化 metadata（PP 硬件通过 metadata->elems->value 查找 blk_id → 物理地址）
    snprintf(blk_id_str_, sizeof(blk_id_str_), "%u", dma_mem_.blk_id);
    dict_entry_.key = const_cast<char*>("pool_blk_id");
    dict_entry_.value = blk_id_str_;
    dict_.count = 1;
    dict_.elems = &dict_entry_;

    LOG4CPLUS_INFO_FMT(logger_,
        "DMA memory allocated: blk_id=%u, phys=0x%llx, virt=%p, size=%zu",
        dma_mem_.blk_id, (unsigned long long)dma_mem_.phys_addr,
        dma_mem_.virt_addr, dma_mem_.total_size);

    return true;
}

void SharedDisplayContext::freeDmaMemory() {
    if (dma_mem_.virt_addr) {
        taco_sys_munmap(dma_mem_.virt_addr, static_cast<uint32_t>(dma_mem_.total_size));
        dma_mem_.virt_addr = nullptr;
    }
    if (dma_mem_.blk_id != 0) {
        taco_sys_release_block(dma_mem_.blk_id);
        dma_mem_.blk_id = 0;
    }
    dma_mem_.phys_addr = 0;
    dma_mem_.total_size = 0;
}

// ============================================================
// DSS DMA 配置
// ============================================================

bool SharedDisplayContext::setupDssForDma() {
    struct tpsfb_dma_info dma_info;
    dma_info.ovl_idx = 0;
    dma_info.phys_addr = dma_mem_.phys_addr;

    if (ioctl(fd_, FB_IOCTL_SET_DMA_INFO, &dma_info) < 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "FB_IOCTL_SET_DMA_INFO failed: %s", strerror(errno));
        return false;
    }

    LOG4CPLUS_INFO_FMT(logger_, "DSS DMA base set to 0x%llx",
        (unsigned long long)dma_mem_.phys_addr);
    return true;
}

// ============================================================
// BufferPool 创建
// ============================================================

bool SharedDisplayContext::createFramebufferPool() {
    allocator_ = std::make_unique<BufferAllocatorFacade>(
        BufferAllocatorFactory::AllocatorType::FRAMEBUFFER);

    if (!allocator_) {
        LOG4CPLUS_ERROR(logger_, "Failed to create BufferAllocatorFacade");
        return false;
    }

    fb_pool_id_ = allocator_->allocatePoolWithBuffers(
        0, 0, "SharedDisplayContext_fb", "Display");

    if (fb_pool_id_ == 0) {
        LOG4CPLUS_ERROR(logger_, "Failed to create BufferPool");
        return false;
    }

    uint8_t* base = static_cast<uint8_t*>(dma_mem_.virt_addr);

    for (int i = 0; i < buffer_count_; i++) {
        void*    virt_addr = base + buffer_size_ * i;
        uint64_t phys_addr = dma_mem_.phys_addr + buffer_size_ * i;

        Buffer* buf = allocator_->injectExternalBufferToPool(
            fb_pool_id_, virt_addr, phys_addr, buffer_size_, QueueType::FREE);

        if (!buf) {
            LOG4CPLUS_ERROR_FMT(logger_, "Failed to inject buffer #%d", i);
            fb_pool_id_ = 0;
            allocator_.reset();
            return false;
        }
    }

    LOG4CPLUS_INFO_FMT(logger_, "BufferPool created with %d framebuffer pages", buffer_count_);
    return true;
}

std::shared_ptr<BufferPool> SharedDisplayContext::getPool() {
    if (fb_pool_id_ == 0) return nullptr;
    return BufferPoolRegistry::getInstance().getPool(fb_pool_id_).lock();
}

// ============================================================
// 通道管理
// ============================================================

int SharedDisplayContext::registerChannel() {
    std::lock_guard<std::mutex> lock(channel_mgmt_mutex_);

    int id = next_channel_id_++;
    ChannelLayout layout;
    computeGridLayout(id, layout);

    ChannelInfo ch;
    ch.channel_id = id;
    ch.x = layout.x;
    ch.y = layout.y;
    ch.w = layout.w;
    ch.h = layout.h;
    ch.active = true;
    channels_.push_back(ch);

    LOG4CPLUS_INFO_FMT(logger_,
        "Channel %d registered (auto): region=(%d,%d,%d,%d)", id, layout.x, layout.y, layout.w, layout.h);

    if (osd_) {
        osd_->registerChannel(id, layout.x, layout.y, layout.w, layout.h);
    }

    return id;
}

int SharedDisplayContext::registerChannel(const ChannelLayout& layout) {
    std::lock_guard<std::mutex> lock(channel_mgmt_mutex_);

    int id = next_channel_id_++;
    ChannelInfo ch;
    ch.channel_id = id;
    ch.x = layout.x;
    ch.y = layout.y;
    ch.w = layout.w;
    ch.h = layout.h;
    ch.active = true;
    channels_.push_back(ch);

    LOG4CPLUS_INFO_FMT(logger_,
        "Channel %d registered: region=(%d,%d,%d,%d)", id, layout.x, layout.y, layout.w, layout.h);

    if (osd_) {
        osd_->registerChannel(id, layout.x, layout.y, layout.w, layout.h);
    }

    return id;
}

void SharedDisplayContext::computeGridLayout(int channel_index, ChannelLayout& layout) const {
    int max_ch = config_.max_channels > 0 ? config_.max_channels : 9;
    int grid_cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(max_ch))));
    int grid_rows = (max_ch + grid_cols - 1) / grid_cols;

    int cell_w = screen_width_ / grid_cols;
    int cell_h = screen_height_ / grid_rows;

    int row = channel_index / grid_cols;
    int col = channel_index % grid_cols;

    layout.x = col * cell_w;
    layout.y = row * cell_h;
    layout.w = cell_w;
    layout.h = cell_h;
}

void SharedDisplayContext::unregisterChannel(int channel_id) {
    {
        std::lock_guard<std::mutex> lock(channel_mgmt_mutex_);
        for (auto& ch : channels_) {
            if (ch.channel_id == channel_id) {
                ch.active = false;
                LOG4CPLUS_INFO_FMT(logger_, "Channel %d unregistered", channel_id);
                if (osd_) {
                    osd_->unregisterChannel(channel_id);
                }
                break;
            }
        }
    }
    // 唤醒可能阻塞在 channelWrite 中的该通道线程
    round_cv_.notify_all();
}

// ============================================================
// 通道写入（条件变量节流：每通道每轮只写一次）
// ============================================================

bool SharedDisplayContext::channelWrite(int channel_id, Buffer* decoded) {
    if (!decoded) return false;

    // 查找通道
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

    // 条件变量等待：如果本轮已经写过，阻塞直到定时器重置标记
    {
        std::unique_lock<std::mutex> round_lock(round_mutex_);
        round_cv_.wait(round_lock, [&]() {
            return !ch_info->written_this_round || !running_ || !ch_info->active;
        });
        if (!running_ || !ch_info->active) return false;
    }

    // 获取 shared_lock 保护 render_buf_（防止定时器 swap 期间写入）
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);

    if (render_buf_ == nullptr) {
        return false;
    }

    int src_width  = decoded->getImageWidth();
    int src_height = decoded->getImageHeight();

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

    ppResize(decoded, render_buf_,
             ch_info->x, ch_info->y, ch_info->w, ch_info->h,
             src_width, src_height, 0, 0, nullptr);

    // 标记本轮已写入
    {
        std::lock_guard<std::mutex> round_lock(round_mutex_);
        ch_info->written_this_round = true;
    }

    if (osd_) {
        osd_->recordFrame(channel_id);
    }

    return true;
}

// ============================================================
// PP 硬件操作
// ============================================================

void SharedDisplayContext::ppResize(
    Buffer* src, Buffer* dst,
    int dst_x, int dst_y, int dst_w, int dst_h,
    int src_width, int src_height,
    uint64_t src_phys, int src_format, const int* src_linesize)
{
    (void)src_phys;
    (void)src_format;
    (void)src_linesize;

    AVFrame* avframe_in = src->getAVFrame();
    if (!avframe_in) {
        LOG4CPLUS_WARN(logger_, "ppResize: decoded buffer has no AVFrame");
        return;
    }

    int bytes_per_pixel = bits_per_pixel_ / 8;
    int out_format = TA_AV_PIX_FMT_NONE;
    if (bits_per_pixel_ == 32) {
        out_format = TA_AV_PIX_FMT_ARGB;
    } else if (bits_per_pixel_ == 24) {
        out_format = TA_AV_PIX_FMT_RGB24;
    } else {
        out_format = TA_AV_PIX_FMT_NV12;
    }

    // === 构建输入 ta_avframe_t（不强制转换 AVFrame*，避免结构体偏移差异）===
    ta_avframe_t in_avframe;
    memset(&in_avframe, 0, sizeof(in_avframe));
    in_avframe.width  = avframe_in->width;
    in_avframe.height = avframe_in->height;
    in_avframe.format = avframe_in->format;
    for (int i = 0; i < TA_AV_NUM_DATA_POINTERS; ++i) {
        in_avframe.data[i]     = avframe_in->data[i];
        in_avframe.linesize[i] = avframe_in->linesize[i];
    }
    // 从 FFmpeg AVFrame 复制 metadata 指针（AVDictionary 与 TA_AVDictionary 布局一致）
    in_avframe.metadata = reinterpret_cast<TA_AVDictionary*>(avframe_in->metadata);

    // === 构建输出 ta_avframe_t（仿照 taco-vo：data[0] 指向 DMA 基地址）===
    TA_AVDictionaryEntry local_out_entry = dict_entry_;
    TA_AVDictionary local_out_dict;
    local_out_dict.count = 1;
    local_out_dict.elems = &local_out_entry;

    ta_avframe_t out_avframe;
    memset(&out_avframe, 0, sizeof(out_avframe));
    out_avframe.width  = screen_width_;
    out_avframe.height = screen_height_;
    out_avframe.format = out_format;
    out_avframe.metadata = &local_out_dict;
    out_avframe.data[0] = static_cast<uint8_t*>(dma_mem_.virt_addr);

    // 计算当前 render buffer 相对 DMA 基地址的页偏移
    // 必须用 phys_addr 计算，因为 freeBuffer() 会将 virt_addr_ 清零
    size_t page_offset = dst->getPhysicalAddress() - dma_mem_.phys_addr;

    // === resize 参数（y_offset 加入页偏移，与 taco-vo send_chn_frame 一致）===
    ta_cv_resize_t resize_params = {};
    resize_params.in_width  = src_width;
    resize_params.in_height = src_height;
    resize_params.out_width = dst_w;
    resize_params.out_height = dst_h;
    resize_params.start_x = 0;
    resize_params.start_y = 0;

    ta_cv_resize_image_t resize_attr = {};
    resize_attr.resize_img_attr = &resize_params;
    resize_attr.interpolation = 1;

    if (out_format == TA_AV_PIX_FMT_NV12) {
        resize_attr.y_offset = dst_y * screen_width_ + dst_x + page_offset;
        resize_attr.u_offset = screen_width_ * (screen_height_ - dst_y)
                             + (screen_width_ / 2) * dst_y;
        resize_attr.y_stride = screen_width_;
        resize_attr.u_stride = screen_width_;
    } else if (out_format == TA_AV_PIX_FMT_RGB24) {
        resize_attr.y_offset = (dst_y * screen_width_ + dst_x) * 3 + page_offset;
        resize_attr.u_offset = 0;
        resize_attr.y_stride = screen_width_ * 3;
        resize_attr.u_stride = screen_width_ * 3;
    } else {
        resize_attr.y_offset = (dst_y * screen_width_ + dst_x) * 4 + page_offset;
        resize_attr.u_offset = 0;
        resize_attr.y_stride = screen_width_ * 4;
        resize_attr.u_stride = screen_width_ * 4;
    }

    int in_format = avframe_in->format;
    bool both_nv12 = (in_format == TA_AV_PIX_FMT_NV12 && out_format == TA_AV_PIX_FMT_NV12);

    ta_image_t image_in = {};
    ta_image_t image_out = {};

    tacv_status_t ret = ta_cv_image_create(0, 0, (ta_image_format_ext_t)0,
        (ta_image_data_format_ext_t)0, &image_in, &in_avframe);
    if (ret != 0) {
        LOG4CPLUS_WARN_FMT(logger_, "ppResize: ta_cv_image_create(input) failed: %d", ret);
        return;
    }

    ret = ta_cv_image_create(0, 0, (ta_image_format_ext_t)0,
        (ta_image_data_format_ext_t)0, &image_out, &out_avframe);
    if (ret != 0) {
        LOG4CPLUS_WARN_FMT(logger_, "ppResize: ta_cv_image_create(output) failed: %d", ret);
        ta_cv_image_destroy(&image_in);
        return;
    }

    if (both_nv12) {
        ret = ta_cv_image_resize(&resize_attr, image_in, image_out);
        if (ret != 0) {
            LOG4CPLUS_WARN_FMT(logger_,
                "ppResize: ta_cv_image_resize failed: ret=%d", ret);
        }
    } else {
        ta_cv_rect_t crop_rect = {};
        crop_rect.crop_w = src_width;
        crop_rect.crop_h = src_height;
        crop_rect.start_x = 0;
        crop_rect.start_y = 0;

        ret = ta_cv_image_csc_convert_to(image_in, image_out, crop_rect,
                                         &resize_attr, CSC_YCbCr2RGB_BT601);
        if (ret != 0) {
            LOG4CPLUS_WARN_FMT(logger_,
                "ppResize: ta_cv_image_csc_convert_to failed: ret=%d", ret);
        }
    }

    ta_cv_image_destroy(&image_in);
    ta_cv_image_destroy(&image_out);
}

void SharedDisplayContext::ppCopy(Buffer* src, Buffer* dst) {
    if (!src || !dst) {
        LOG4CPLUS_ERROR_FMT(logger_, "ppCopy: null buffer src=%p dst=%p",
            (void*)src, (void*)dst);
        return;
    }

    // 通过物理地址计算虚拟地址（freeBuffer() 会清除 virt_addr_ 但保留 phys_addr_）
    uint8_t* dma_base = static_cast<uint8_t*>(dma_mem_.virt_addr);
    size_t src_offset = src->getPhysicalAddress() - dma_mem_.phys_addr;
    size_t dst_offset = dst->getPhysicalAddress() - dma_mem_.phys_addr;

    if (src_offset >= dma_mem_.total_size || dst_offset >= dma_mem_.total_size) {
        LOG4CPLUS_ERROR_FMT(logger_,
            "ppCopy: buffer outside DMA range src_off=%zu dst_off=%zu total=%zu",
            src_offset, dst_offset, dma_mem_.total_size);
        return;
    }

    int bytes_per_pixel = bits_per_pixel_ / 8;
    int out_format = TA_AV_PIX_FMT_NONE;
    if (bits_per_pixel_ == 32) {
        out_format = TA_AV_PIX_FMT_ARGB;
    } else if (bits_per_pixel_ == 24) {
        out_format = TA_AV_PIX_FMT_RGB24;
    } else {
        out_format = TA_AV_PIX_FMT_NV12;
    }

    // ppCopy：data[0] 指向各自页的虚拟地址（通过 phys 偏移计算，绕过 freeBuffer() 清除问题）
    uint8_t* src_virt = dma_base + src_offset;
    uint8_t* dst_virt = dma_base + dst_offset;

    TA_AVDictionaryEntry local_entry = dict_entry_;
    TA_AVDictionary local_dict;
    local_dict.count = 1;
    local_dict.elems = &local_entry;

    TA_AVDictionaryEntry local_entry2 = dict_entry_;
    TA_AVDictionary local_dict2;
    local_dict2.count = 1;
    local_dict2.elems = &local_entry2;

    ta_avframe_t src_avframe;
    memset(&src_avframe, 0, sizeof(src_avframe));
    src_avframe.width  = screen_width_;
    src_avframe.height = screen_height_;
    src_avframe.format = out_format;
    src_avframe.metadata = &local_dict;
    src_avframe.data[0] = src_virt;

    ta_avframe_t dst_avframe;
    memset(&dst_avframe, 0, sizeof(dst_avframe));
    dst_avframe.width  = screen_width_;
    dst_avframe.height = screen_height_;
    dst_avframe.format = out_format;
    dst_avframe.metadata = &local_dict2;
    dst_avframe.data[0] = dst_virt;

    ta_cv_resize_t resize_params = {};
    resize_params.in_width  = screen_width_;
    resize_params.in_height = screen_height_;
    resize_params.out_width = screen_width_;
    resize_params.out_height = screen_height_;
    resize_params.start_x = 0;
    resize_params.start_y = 0;

    ta_cv_resize_image_t resize_attr = {};
    resize_attr.resize_img_attr = &resize_params;
    resize_attr.interpolation = 1;

    if (out_format == TA_AV_PIX_FMT_NV12) {
        // u_offset 相对于 y_offset（即 Y 写入位置），需要跳过 Y 平面到达 UV 平面
        resize_attr.u_offset = screen_width_ * screen_height_;
        resize_attr.y_stride = screen_width_;
        resize_attr.u_stride = screen_width_;
    } else {
        resize_attr.y_stride = screen_width_ * bytes_per_pixel;
        resize_attr.u_stride = screen_width_ * bytes_per_pixel;
    }

    ta_image_t image_in = {};
    ta_image_t image_out = {};

    tacv_status_t ret = ta_cv_image_create(0, 0, (ta_image_format_ext_t)0,
        (ta_image_data_format_ext_t)0, &image_in, &src_avframe);
    if (ret != 0) {
        LOG4CPLUS_WARN_FMT(logger_, "ppCopy: ta_cv_image_create(input) failed: %d", ret);
        memcpy(dst_virt, src_virt, buffer_size_);
        return;
    }

    ret = ta_cv_image_create(0, 0, (ta_image_format_ext_t)0,
        (ta_image_data_format_ext_t)0, &image_out, &dst_avframe);
    if (ret != 0) {
        LOG4CPLUS_WARN_FMT(logger_, "ppCopy: ta_cv_image_create(output) failed: %d", ret);
        ta_cv_image_destroy(&image_in);
        memcpy(dst_virt, src_virt, buffer_size_);
        return;
    }

    ret = ta_cv_image_resize(&resize_attr, image_in, image_out);
    if (ret != 0) {
        LOG4CPLUS_WARN_FMT(logger_, "ppCopy: ta_cv_image_resize failed: ret=%d, fallback to memcpy", ret);
        memcpy(dst_virt, src_virt, buffer_size_);
    }

    ta_cv_image_destroy(&image_in);
    ta_cv_image_destroy(&image_out);
}

// ============================================================
// 定时器线程
// ============================================================

bool SharedDisplayContext::startThreads() {
    timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (timer_fd_ < 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "timerfd_create failed: %s", strerror(errno));
        return false;
    }

    int fps = config_.target_fps > 0 ? config_.target_fps : 30;
    long interval_ns = 1000000000L / fps;

    struct itimerspec ts = {};
    ts.it_interval.tv_sec  = interval_ns / 1000000000L;
    ts.it_interval.tv_nsec = interval_ns % 1000000000L;
    ts.it_value = ts.it_interval;

    if (timerfd_settime(timer_fd_, 0, &ts, nullptr) < 0) {
        LOG4CPLUS_ERROR_FMT(logger_, "timerfd_settime failed: %s", strerror(errno));
        close(timer_fd_);
        timer_fd_ = -1;
        return false;
    }

    running_ = true;
    timer_thread_   = std::thread(&SharedDisplayContext::timerThreadFunc, this);
    display_thread_ = std::thread(&SharedDisplayContext::displayThreadFunc, this);

    LOG4CPLUS_INFO_FMT(logger_, "Timer and display threads started (target_fps=%d)", fps);
    return true;
}

void SharedDisplayContext::stopThreads() {
    running_ = false;

    // 唤醒所有阻塞在条件变量上的通道线程
    round_cv_.notify_all();

    if (timer_fd_ >= 0) {
        struct itimerspec ts = {};
        timerfd_settime(timer_fd_, 0, &ts, nullptr);
    }

    // 唤醒显示线程（可能阻塞在 acquireFilled）
    auto pool = getPool();
    if (pool) {
        pool->shutdown();
    }

    if (timer_thread_.joinable()) {
        timer_thread_.join();
    }
    if (display_thread_.joinable()) {
        display_thread_.join();
    }

    if (timer_fd_ >= 0) {
        close(timer_fd_);
        timer_fd_ = -1;
    }
}

void SharedDisplayContext::timerThreadFunc() {
    LOG4CPLUS_DEBUG(logger_, "Timer thread started");
    auto pool = getPool();
    if (!pool) {
        LOG4CPLUS_ERROR(logger_, "Timer thread: pool is null");
        return;
    }

    while (running_) {
        uint64_t expirations = 0;
        ssize_t s = read(timer_fd_, &expirations, sizeof(expirations));
        if (s != sizeof(expirations)) {
            if (!running_) break;
            continue;
        }

        Buffer* old_render = nullptr;

        // Step 1: 获取独占锁 → 等待所有正在进行的 PP resize 完成
        {
            std::unique_lock<std::shared_mutex> lock(rw_mutex_);
            old_render = render_buf_;
            render_buf_ = nullptr;
        }

        if (!old_render) {
            continue;
        }

        // Step 2: 尝试获取空闲 buffer
        Buffer* new_render = pool->acquireFree(false, 0);
        if (!new_render) {
            std::unique_lock<std::shared_mutex> lock(rw_mutex_);
            render_buf_ = old_render;
            // 即使没拿到新 buffer，也要重置标记唤醒通道，
            // 否则通道会永远阻塞
            {
                std::lock_guard<std::mutex> round_lock(round_mutex_);
                for (auto& ch : channels_) {
                    ch.written_this_round = false;
                }
            }
            round_cv_.notify_all();
            continue;
        }

        // Step 3: 提交旧 buffer 到 FILLED 队列
        pool->submitFilled(old_render);

        // Step 4: PP 硬件拷贝陈旧区域（display_buf_ → new_render）
        // 在 display_mutex_ 保护下读取 display_buf_，
        // 防止 displayThreadFunc 在 ppCopy 期间释放该 buffer
        // 注：display_buf_ 为 nullptr 仅发生在首次定时器 tick（DMA 内存已在 allocateDmaMemory 中清零）
        {
            std::lock_guard<std::mutex> dlock(display_mutex_);
            if (display_buf_) {
                ppCopy(display_buf_, new_render);
            }
        }

        // Step 5: 设置新的渲染目标
        {
            std::unique_lock<std::shared_mutex> lock(rw_mutex_);
            render_buf_ = new_render;
        }

        // Step 6: 重置所有通道的写入标记，唤醒等待的通道线程
        {
            std::lock_guard<std::mutex> round_lock(round_mutex_);
            for (auto& ch : channels_) {
                ch.written_this_round = false;
            }
        }
        round_cv_.notify_all();
    }

    LOG4CPLUS_DEBUG(logger_, "Timer thread exited");
}

// ============================================================
// 显示线程
// ============================================================

void SharedDisplayContext::displayThreadFunc() {
    LOG4CPLUS_DEBUG(logger_, "Display thread started");
    auto pool = getPool();
    if (!pool) {
        LOG4CPLUS_ERROR(logger_, "Display thread: pool is null");
        return;
    }

    while (running_) {
        Buffer* buf = pool->acquireFilled(true, 100);
        if (!buf) {
            continue;
        }

        // FBIOPAN_DISPLAY: 切换显示到此 buffer
        struct fb_var_screeninfo var_info;
        if (ioctl(fd_, FBIOGET_VSCREENINFO, &var_info) < 0) {
            LOG4CPLUS_WARN_FMT(logger_, "Display: FBIOGET_VSCREENINFO failed: %s", strerror(errno));
            pool->releaseFilled(buf);
            continue;
        }

        var_info.yoffset = var_info.yres * buf->id();
        if (ioctl(fd_, FBIOPAN_DISPLAY, &var_info) < 0) {
            LOG4CPLUS_WARN_FMT(logger_, "Display: FBIOPAN_DISPLAY failed: %s", strerror(errno));
            pool->releaseFilled(buf);
            continue;
        }

        // VSYNC: 等待垂直消隐期
        int zero = 0;
        ioctl(fd_, FBIO_WAITFORVSYNC, &zero);

        // 在 display_mutex_ 保护下切换 display_buf_，
        // 防止 timerThreadFunc 在 ppCopy 期间读到过时指针
        Buffer* old_display = nullptr;
        {
            std::lock_guard<std::mutex> dlock(display_mutex_);
            old_display = display_buf_;
            display_buf_ = buf;
        }

        if (old_display) {
            pool->releaseFilled(old_display);
        }
    }

    LOG4CPLUS_DEBUG(logger_, "Display thread exited");
}
