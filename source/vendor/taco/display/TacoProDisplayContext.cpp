#include "vendor/taco/display/TacoProDisplayContext.hpp"
#include "vendor/taco/display/TacoProOsdOverlay.hpp"
#include "common/Logger.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <cstring>
#include <cerrno>
#include <cmath>
#include <array>
#include <sstream>
#include <algorithm>

extern "C" {
#include "taco_sys_api.h"
}

namespace {
    constexpr const char* kProcFb = "/proc/fb";
    constexpr const char* kTpsFb0 = "tpsfb0";

    constexpr std::array<const char*, 3> kDevFbPaths = {{
        "/dev/fb0",
        "/dev/fb1",
        "/dev/fb2",
    }};

    int selectGridCount(int max_channels) {
        constexpr int presets[] = {1, 2, 4, 9, 16, 25, 36};
        for (int g : presets) {
            if (max_channels <= g) return g;
        }
        int n = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(max_channels))));
        return n * n;
    }

    void computeGridSlots(int count, int screen_w, int screen_h,
                          std::vector<TacoProDisplayContext::ChannelLayout>& slots) {
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
                                  std::vector<TacoProDisplayContext::ChannelLayout>& slots) {
        int main_w = static_cast<int>(screen_w * ratio);
        int side_w = screen_w - main_w;
        int side_h = screen_h / 4;

        slots.resize(5);
        slots[0] = {0, 0, main_w, screen_h};
        for (int i = 0; i < 4; ++i) {
            slots[1 + i] = {main_w, i * side_h, side_w, side_h};
        }
    }
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
    , render_buf_(nullptr)
    , displayed_buf_(nullptr)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Display.SharedContext")))
{
    channels_.reserve(64);
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

    createView();

    if (!createBufferPool()) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    auto pool = getBufferPool();
    if (!pool) {
        fb_pool_id_ = 0;
        allocator_facade_.reset();
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
        allocator_facade_.reset();
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
        allocator_facade_.reset();
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    memset(tmpl_virt, 0, buffer_size_);
    template_buf_ = std::make_unique<Buffer>(
        template_blk_id_, tmpl_virt, tmpl_phys, buffer_size_,
        Buffer::Ownership::EXTERNAL);

    LOG4CPLUS_INFO_FMT(logger_,
        "Template frame allocated: blk_id=%u, phys=0x%llx, size=%zu",
        template_blk_id_, (unsigned long long)tmpl_phys, buffer_size_);

    if (!startThreads()) {
        template_buf_.reset();
        taco_sys_munmap(tmpl_virt, static_cast<uint32_t>(buffer_size_));
        taco_sys_release_block(template_blk_id_);
        template_blk_id_ = 0;
        fb_pool_id_ = 0;
        allocator_facade_.reset();
        ::close(fd_);
        fd_ = -1;
        return false;
    }

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
        (view_type_ == ViewType::GRID ? "grid" : "main_sidebar"),
        osd_ ? "on" : "off");

    LOG4CPLUS_INFO(logger_, "View layout:\n" << getViewDiagram());

    return true;
}

void TacoProDisplayContext::close() {
    if (osd_) {
        osd_->shutdown();
        osd_.reset();
    }

    stopThreads();

    render_buf_ = nullptr;

    // 释放显示定时器持有的帧（上一次 onDisplayTick 保留的 buffer）
    if (displayed_buf_) {
        auto pool = getBufferPool();
        if (pool) {
            pool->releaseFilled(displayed_buf_);
        }
        displayed_buf_ = nullptr;
    }

    // 释放模板帧（独立 TACO 内存）
    if (template_buf_) {
        void* virt = template_buf_->getVirtualAddress();
        if (virt) {
            taco_sys_munmap(virt, static_cast<uint32_t>(buffer_size_));
        }
        template_buf_.reset();
    }
    if (template_blk_id_ != 0) {
        taco_sys_release_block(template_blk_id_);
        template_blk_id_ = 0;
    }

    // destroyPool 内部自动 taco_sys_munmap + taco_sys_release_block
    fb_pool_id_ = 0;
    allocator_facade_.reset();

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
        LOG4CPLUS_ERROR(logger_, "tpsfb0 not found in /proc/fb");
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
    allocator_facade_ = std::make_unique<BufferAllocatorFacade>(
        BufferAllocatorFactory::AllocatorType::FRAMEBUFFER);

    fb_pool_id_ = allocator_facade_->allocatePoolWithBuffers(
        buffer_count_, buffer_size_, "TacoProDisplayContext_fb", "Display");
    if (fb_pool_id_ == 0) {
        LOG4CPLUS_ERROR(logger_, "Failed to create BufferPool");
        allocator_facade_.reset();
        return false;
    }

    return true;
}

std::shared_ptr<BufferPool> TacoProDisplayContext::getBufferPool() {
    if (fb_pool_id_ == 0) return nullptr;
    return BufferPoolRegistry::getInstance().getPool(fb_pool_id_).lock();
}

// ============================================================
// 视图管理
// ============================================================

void TacoProDisplayContext::createView() {
    if (config_.view_type == "main_sidebar") {
        view_type_ = ViewType::MAIN_SIDEBAR;
        computeMainSidebarSlots(screen_width_, screen_height_,
                                config_.main_sidebar_ratio, view_slots_);
    } else {
        view_type_ = ViewType::GRID;
        int grid_count = selectGridCount(9);
        computeGridSlots(grid_count, screen_width_, screen_height_, view_slots_);
    }

    slot_assignment_ = config_.slot_assignment;

    LOG4CPLUS_INFO_FMT(logger_, "View created: type=%s, slots=%d, assignment_size=%d",
        (view_type_ == ViewType::GRID ? "grid" : "main_sidebar"),
        static_cast<int>(view_slots_.size()),
        static_cast<int>(slot_assignment_.size()));
}

const TacoProDisplayContext::ChannelLayout&
TacoProDisplayContext::resolveLayout(int channel_id) const {
    if (!slot_assignment_.empty()) {
        for (int i = 0; i < static_cast<int>(slot_assignment_.size()); ++i) {
            if (slot_assignment_[i] == channel_id) {
                return view_slots_.at(i);
            }
        }
    }
    return view_slots_.at(channel_id);
}

const TacoProDisplayContext::ChannelLayout&
TacoProDisplayContext::getSlotLayout(int slot_index) const {
    return view_slots_.at(slot_index);
}

// ============================================================
// 通道管理
// ============================================================

int TacoProDisplayContext::registerChannel() {
    std::lock_guard<std::mutex> lock(channel_mgmt_mutex_);

    int id = next_channel_id_++;

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

    if (osd_) {
        osd_->registerChannel(id, layout.x, layout.y, layout.w, layout.h);
    }

    return id;
}

int TacoProDisplayContext::registerChannel(const ChannelLayout& layout) {
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

    if (osd_) {
        osd_->registerChannel(id, layout.x, layout.y, layout.w, layout.h);
    }

    return id;
}

void TacoProDisplayContext::unregisterChannel(int channel_id) {
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
// 视图展示
// ============================================================

std::string TacoProDisplayContext::getViewDiagram() const {
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
            // 坐标行（网格顶部水平线）
            for (int c = 0; c <= cols; ++c) {
                std::string pt = coord(c * cell_w, r * cell_h);
                oss << pt;
                if (c < cols) {
                    size_t fill = cell_text_w - pt.size();
                    for (size_t i = 0; i < fill; ++i) oss << "\xe2\x94\x80"; // ─
                }
            }
            oss << "\n";

            // 单元格内容行：通道信息
            int slot_base = r * cols;
            // 第一行：竖线 + 空格
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

        // 最后一行坐标（底部边线）
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

        // 顶部边线
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

            // 3 行内容
            for (int line = 0; line < 3; ++line) {
                // 左列（主画面）
                oss << "\xe2\x94\x82"; // │
                std::string left_content;
                if (i == 1 && line == 1) {
                    left_content = "      [CH " + std::to_string(main_ch_id) + "]";
                } else if (i == 2 && line == 1) {
                    left_content = "      " + std::to_string(main_w) + "x" + std::to_string(screen_height_);
                }
                size_t lpad = (left_content.size() < main_col_w - 1) ? main_col_w - 1 - left_content.size() : 0;
                oss << left_content << std::string(lpad, ' ');

                // 右列（侧栏通道）
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

            // 侧栏分隔线
            if (i < 3) {
                // 主画面左侧只有竖线，侧栏有水平线
                oss << "\xe2\x94\x82"; // │
                oss << std::string(main_col_w - 1, ' ');
                std::string mid_pt = coord(main_w, (i + 1) * side_h);
                oss << mid_pt;
                for (size_t j = mid_pt.size(); j < side_col_w; ++j) oss << "\xe2\x94\x80";
                oss << coord(main_w + side_w, (i + 1) * side_h) << "\n";
            }
        }

        // 底部边线
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

// ============================================================
// 通道写入（等待渲染线程开启新一轮，每通道每轮只写一次）
// ============================================================

bool TacoProDisplayContext::channelWrite(int channel_id, Buffer* decoded) {
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

    // 等待渲染线程开启新一轮，然后尝试写入 render_buf_
    // 使用轮次号 round_seq_ 判断新一轮是否开始，避免 render_buf_ 为空时忙等
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
                // 本轮已结束（渲染线程已清空 render_buf_），等待下一轮
                // 不设置 written_this_round = true，确保渲染线程仍会为此通道执行 copyTemplateRegion
                // round_cv_.wait 会阻塞直到 round_seq_ > my_round（下一轮开始）
                continue;
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
                     ch_info->layout.x, ch_info->layout.y,
                     ch_info->layout.w, ch_info->layout.h,
                     src_width, src_height, 0, 0, nullptr);
        }
        break;
    }

    // 标记本轮已写入，通知渲染线程
    {
        std::lock_guard<std::mutex> round_lock(round_mutex_);
        ch_info->written_this_round = true;
    }
    render_cv_.notify_one();

    if (osd_) {
        osd_->recordFrame(channel_id);
    }

    return true;
}

// ============================================================
// PP 硬件操作
// ============================================================

void TacoProDisplayContext::ppResize(
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

    // === 构建输出 ta_avframe_t（buf->id() == blk_id，用于 PP 物理地址查找）===
    char blk_id_str[16];
    snprintf(blk_id_str, sizeof(blk_id_str), "%u", dst->id());
    TA_AVDictionaryEntry local_out_entry;
    local_out_entry.key = const_cast<char*>("pool_blk_id");
    local_out_entry.value = blk_id_str;
    TA_AVDictionary local_out_dict;
    local_out_dict.count = 1;
    local_out_dict.elems = &local_out_entry;

    ta_avframe_t out_avframe;
    memset(&out_avframe, 0, sizeof(out_avframe));
    out_avframe.width  = screen_width_;
    out_avframe.height = screen_height_;
    out_avframe.format = out_format;
    out_avframe.metadata = &local_out_dict;
    out_avframe.data[0] = static_cast<uint8_t*>(dst->getVirtualAddress());

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
        resize_attr.y_offset = dst_y * screen_width_ + dst_x;
        resize_attr.u_offset = screen_width_ * (screen_height_ - dst_y)
                             + (screen_width_ / 2) * dst_y;
        resize_attr.y_stride = screen_width_;
        resize_attr.u_stride = screen_width_;
    } else if (out_format == TA_AV_PIX_FMT_RGB24) {
        resize_attr.y_offset = (dst_y * screen_width_ + dst_x) * 3;
        resize_attr.u_offset = 0;
        resize_attr.y_stride = screen_width_ * 3;
        resize_attr.u_stride = screen_width_ * 3;
    } else {
        resize_attr.y_offset = (dst_y * screen_width_ + dst_x) * 4;
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

void TacoProDisplayContext::ppCopy(Buffer* src, Buffer* dst) {
    if (!src || !dst) {
        LOG4CPLUS_ERROR_FMT(logger_, "ppCopy: null buffer src=%p dst=%p",
            (void*)src, (void*)dst);
        return;
    }

    uint8_t* src_virt = static_cast<uint8_t*>(src->getVirtualAddress());
    uint8_t* dst_virt = static_cast<uint8_t*>(dst->getVirtualAddress());

    int bytes_per_pixel = bits_per_pixel_ / 8;
    int out_format = TA_AV_PIX_FMT_NONE;
    if (bits_per_pixel_ == 32) {
        out_format = TA_AV_PIX_FMT_ARGB;
    } else if (bits_per_pixel_ == 24) {
        out_format = TA_AV_PIX_FMT_RGB24;
    } else {
        out_format = TA_AV_PIX_FMT_NV12;
    }

    char src_blk_str[16], dst_blk_str[16];
    snprintf(src_blk_str, sizeof(src_blk_str), "%u", src->id());
    snprintf(dst_blk_str, sizeof(dst_blk_str), "%u", dst->id());

    TA_AVDictionaryEntry local_entry;
    local_entry.key = const_cast<char*>("pool_blk_id");
    local_entry.value = src_blk_str;
    TA_AVDictionary local_dict;
    local_dict.count = 1;
    local_dict.elems = &local_entry;

    TA_AVDictionaryEntry local_entry2;
    local_entry2.key = const_cast<char*>("pool_blk_id");
    local_entry2.value = dst_blk_str;
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

void TacoProDisplayContext::copyTemplateRegion(Buffer* dst, const ChannelLayout& layout) {
    if (!template_buf_ || !dst) return;

    int out_format = TA_AV_PIX_FMT_NONE;
    if (bits_per_pixel_ == 32) {
        out_format = TA_AV_PIX_FMT_ARGB;
    } else if (bits_per_pixel_ == 24) {
        out_format = TA_AV_PIX_FMT_RGB24;
    } else {
        out_format = TA_AV_PIX_FMT_NV12;
    }

    // 构建输入 ta_avframe_t（template_buf_ 区域作为源）
    char src_blk_str[16];
    snprintf(src_blk_str, sizeof(src_blk_str), "%u", template_blk_id_);
    TA_AVDictionaryEntry src_entry;
    src_entry.key   = const_cast<char*>("pool_blk_id");
    src_entry.value = src_blk_str;
    TA_AVDictionary src_dict;
    src_dict.count = 1;
    src_dict.elems = &src_entry;

    ta_avframe_t in_avframe;
    memset(&in_avframe, 0, sizeof(in_avframe));
    in_avframe.width    = layout.w;
    in_avframe.height   = layout.h;
    in_avframe.format   = out_format;
    in_avframe.metadata = &src_dict;

    uint8_t* src_base = static_cast<uint8_t*>(template_buf_->getVirtualAddress());
    if (!src_base) return;

    if (out_format == TA_AV_PIX_FMT_NV12) {
        in_avframe.data[0]     = src_base + layout.y * screen_width_ + layout.x;
        in_avframe.linesize[0] = screen_width_;
        in_avframe.data[1]     = src_base + screen_width_ * screen_height_
                                 + (layout.y / 2) * screen_width_ + layout.x;
        in_avframe.linesize[1] = screen_width_;
    } else {
        int bpp = bits_per_pixel_ / 8;
        in_avframe.data[0]     = src_base + (layout.y * screen_width_ + layout.x) * bpp;
        in_avframe.linesize[0] = screen_width_ * bpp;
    }

    // 构建输出 ta_avframe_t（dst buffer 的同一区域）
    char dst_blk_str[16];
    snprintf(dst_blk_str, sizeof(dst_blk_str), "%u", dst->id());
    TA_AVDictionaryEntry dst_entry;
    dst_entry.key   = const_cast<char*>("pool_blk_id");
    dst_entry.value = dst_blk_str;
    TA_AVDictionary dst_dict;
    dst_dict.count = 1;
    dst_dict.elems = &dst_entry;

    ta_avframe_t out_avframe;
    memset(&out_avframe, 0, sizeof(out_avframe));
    out_avframe.width    = screen_width_;
    out_avframe.height   = screen_height_;
    out_avframe.format   = out_format;
    out_avframe.metadata = &dst_dict;
    out_avframe.data[0]  = static_cast<uint8_t*>(dst->getVirtualAddress());

    ta_cv_resize_t resize_params = {};
    resize_params.in_width   = layout.w;
    resize_params.in_height  = layout.h;
    resize_params.out_width  = layout.w;
    resize_params.out_height = layout.h;
    resize_params.start_x    = 0;
    resize_params.start_y    = 0;

    ta_cv_resize_image_t resize_attr = {};
    resize_attr.resize_img_attr = &resize_params;
    resize_attr.interpolation   = 1;

    if (out_format == TA_AV_PIX_FMT_NV12) {
        resize_attr.y_offset = layout.y * screen_width_ + layout.x;
        resize_attr.u_offset = screen_width_ * (screen_height_ - layout.y)
                             + (screen_width_ / 2) * layout.y;
        resize_attr.y_stride = screen_width_;
        resize_attr.u_stride = screen_width_;
    } else if (out_format == TA_AV_PIX_FMT_RGB24) {
        resize_attr.y_offset = (layout.y * screen_width_ + layout.x) * 3;
        resize_attr.u_offset = 0;
        resize_attr.y_stride = screen_width_ * 3;
        resize_attr.u_stride = screen_width_ * 3;
    } else {
        resize_attr.y_offset = (layout.y * screen_width_ + layout.x) * 4;
        resize_attr.u_offset = 0;
        resize_attr.y_stride = screen_width_ * 4;
        resize_attr.u_stride = screen_width_ * 4;
    }

    ta_image_t image_in = {}, image_out = {};
    tacv_status_t ret = ta_cv_image_create(0, 0, (ta_image_format_ext_t)0,
        (ta_image_data_format_ext_t)0, &image_in, &in_avframe);
    if (ret != 0) {
        LOG4CPLUS_WARN_FMT(logger_,
            "copyTemplateRegion: ta_cv_image_create(input) failed: %d", ret);
        return;
    }

    ret = ta_cv_image_create(0, 0, (ta_image_format_ext_t)0,
        (ta_image_data_format_ext_t)0, &image_out, &out_avframe);
    if (ret != 0) {
        LOG4CPLUS_WARN_FMT(logger_,
            "copyTemplateRegion: ta_cv_image_create(output) failed: %d", ret);
        ta_cv_image_destroy(&image_in);
        return;
    }

    ret = ta_cv_image_resize(&resize_attr, image_in, image_out);
    if (ret != 0) {
        LOG4CPLUS_WARN_FMT(logger_,
            "copyTemplateRegion: ta_cv_image_resize failed: ret=%d (ch region %d,%d %dx%d)",
            ret, layout.x, layout.y, layout.w, layout.h);
    }

    ta_cv_image_destroy(&image_in);
    ta_cv_image_destroy(&image_out);
}

// ============================================================
// 渲染线程 & 显示定时器
// ============================================================

bool TacoProDisplayContext::startThreads() {
    int fps = config_.target_fps > 0 ? config_.target_fps : 30;
    int display_interval_ms = 1000 / fps;
    frame_timeout_ms_ = display_interval_ms * 2;

    running_ = true;

    render_thread_ = std::thread(&TacoProDisplayContext::renderThreadFunc, this);

    timer_.start();
    timer_id_ = timer_.scheduleRepeated(display_interval_ms,
                                        [this]() { onDisplayTick(); });

    LOG4CPLUS_INFO_FMT(logger_,
        "Render thread and display timer started (fps=%d, display_interval=%dms, frame_timeout=%dms)",
        fps, display_interval_ms, frame_timeout_ms_);
    return true;
}

void TacoProDisplayContext::stopThreads() {
    running_ = false;

    round_cv_.notify_all();
    render_cv_.notify_all();

    if (timer_id_ != 0) {
        timer_.cancel(timer_id_);
        timer_id_ = 0;
    }
    timer_.stop();

    auto pool = getBufferPool();
    if (pool) {
        pool->shutdown();
    }

    if (render_thread_.joinable()) {
        render_thread_.join();
    }
}

void TacoProDisplayContext::renderThreadFunc() {
    LOG4CPLUS_DEBUG(logger_, "Render thread started");
    auto pool = getBufferPool();
    if (!pool) {
        LOG4CPLUS_ERROR(logger_, "Render thread: pool is null");
        return;
    }

    while (running_) {
        // 无活跃通道时短暂休眠，避免空转
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

        // (1) 从 BufferPool 获取空闲 buffer
        Buffer* buf = pool->acquireFree(true, 100);
        if (!buf) continue;

        // (2) 用 template 整帧初始化 buf，确保所有区域（含通道间隙）都有完整内容
        ppCopy(template_buf_.get(), buf);

        // (3) 设置 render_buf_，通道可以开始写入
        {
            std::unique_lock<std::shared_mutex> lock(rw_mutex_);
            render_buf_ = buf;
        }

        // (4) 递增轮次号，重置所有通道标记，唤醒通道线程
        {
            std::lock_guard<std::mutex> round_lock(round_mutex_);
            round_seq_++;
            for (auto& ch : channels_) {
                if (ch.active) ch.written_this_round = false;
            }
        }
        round_cv_.notify_all();

        // (5) 等待所有活跃通道写完 OR 帧超时
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
            pool->releaseFree(buf);
            break;
        }

        // (6) 断开 render_buf_（独占锁等待所有正在执行的 ppResize 完成）
        {
            std::unique_lock<std::shared_mutex> lock(rw_mutex_);
            render_buf_ = nullptr;
        }

        // (7) 检查超时通道（buf 已在步骤 2 用 template 整帧初始化，无需再做子区域拷贝）
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

        // (8) 保存当前帧到模板，然后提交到 FILLED 队列供显示定时器取用
        ppCopy(buf, template_buf_.get());
        pool->submitFilled(buf);
    }

    LOG4CPLUS_DEBUG(logger_, "Render thread exited");
}

// ============================================================
// 显示定时器回调
// ============================================================

void TacoProDisplayContext::onDisplayTick() {
    if (!running_) return;

    auto pool = getBufferPool();
    if (!pool) return;

    Buffer* buf = pool->acquireFilled(false, 0);
    if (!buf) return;

    struct tpsfb_dma_info dma_info;
    dma_info.ovl_idx = 0;
    dma_info.phys_addr = buf->getPhysicalAddress();
    if (ioctl(fd_, FB_IOCTL_SET_DMA_INFO, &dma_info) < 0) {
        LOG4CPLUS_WARN_FMT(logger_,
            "Display: FB_IOCTL_SET_DMA_INFO failed: %s", strerror(errno));
        pool->releaseFilled(buf);
        return;
    }

    struct fb_var_screeninfo var_info;
    if (ioctl(fd_, FBIOGET_VSCREENINFO, &var_info) < 0) {
        LOG4CPLUS_WARN_FMT(logger_,
            "Display: FBIOGET_VSCREENINFO failed: %s", strerror(errno));
        pool->releaseFilled(buf);
        return;
    }

    var_info.yoffset = 0;
    if (ioctl(fd_, FBIOPAN_DISPLAY, &var_info) < 0) {
        LOG4CPLUS_WARN_FMT(logger_,
            "Display: FBIOPAN_DISPLAY failed: %s", strerror(errno));
        pool->releaseFilled(buf);
        return;
    }

    int zero = 0;
    ioctl(fd_, FBIO_WAITFORVSYNC, &zero);

    if (displayed_buf_) {
        pool->releaseFilled(displayed_buf_);
    }
    displayed_buf_ = buf;
}
