#include "../../include/decoder/TacoHardwareDecoder.hpp"
#include "../../include/buffer/BufferPoolRegistry.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/opt.h>
}

TacoHardwareDecoder::TacoHardwareDecoder()
    : FFmpegDecoder()
    , overlay_count_(0)
    , ch0_enabled_(true)
    , ch1_enabled_(true)
    , ch1_rgb_format_("argb888")
    , ch1_rgb_std_("bt601")
{
}

TacoHardwareDecoder::~TacoHardwareDecoder() {
    cleanupAVFramePool();
}

DecoderStatus TacoHardwareDecoder::initializeWithOverlayPool(
    const DecoderConfig& config, 
    int overlay_count) {
    
    overlay_count_ = overlay_count;
    
    printf("🚀 Initializing TacoHardwareDecoder with overlay pool...\n");
    
    // 1. 配置 Taco 专用选项（在初始化解码器之前）
    DecoderStatus status = configureTacoOptions();
    if (status != DecoderStatus::OK) {
        return status;
    }
    
    // 2. 初始化 FFmpeg 解码器（调用父类）
    status = initialize(config);
    if (status != DecoderStatus::OK) {
        return status;
    }
    
    // 3. 创建 overlay BufferPool
    if (!createOverlayPool(overlay_count)) {
        setError("Failed to create overlay BufferPool");
        return DecoderStatus::OUT_OF_MEMORY;
    }
    
    // 4. 创建 AVFrame 池（关键！）
    if (!createAVFramePool(overlay_count)) {
        setError("Failed to create AVFrame pool");
        return DecoderStatus::OUT_OF_MEMORY;
    }
    
    printf("✅ TacoHardwareDecoder initialized successfully\n");
    printf("   Overlay count: %d\n", overlay_count);
    printf("   AVFrame pool: %d AVFrames pre-allocated\n", overlay_count);
    printf("   BufferPool name: '%s'\n", getOverlayPoolName().c_str());
    printf("   Dual channel: ch0=%d, ch1=%d, format=%s, std=%s\n",
           ch0_enabled_, ch1_enabled_, ch1_rgb_format_.c_str(), ch1_rgb_std_.c_str());
    
    return DecoderStatus::OK;
}

DecoderStatus TacoHardwareDecoder::configureTacoOptions() {
    // 这里只是保存配置，实际的 av_opt_set 会在 initialize() 中的 codec_ctx 上设置
    // 因为需要等到 codec_ctx 创建后才能设置选项
    
    printf("📝 Taco decoder options configured:\n");
    printf("   ch0_enable: %d\n", ch0_enabled_);
    printf("   ch1_enable: %d\n", ch1_enabled_);
    printf("   ch1_rgb_format: %s\n", ch1_rgb_format_.c_str());
    printf("   ch1_rgb_std: %s\n", ch1_rgb_std_.c_str());
    
    return DecoderStatus::OK;
}

DecoderStatus TacoHardwareDecoder::configureDualChannel(
    bool ch0_enable,
    bool ch1_enable,
    const char* ch1_rgb_format,
    const char* ch1_rgb_std) {
    
    ch0_enabled_ = ch0_enable;
    ch1_enabled_ = ch1_enable;
    ch1_rgb_format_ = ch1_rgb_format;
    ch1_rgb_std_ = ch1_rgb_std;
    
    return DecoderStatus::OK;
}

bool TacoHardwareDecoder::createOverlayPool(int overlay_count) {
    // 创建"虚拟" Buffer（不分配真实内存）
    std::vector<BufferPool::ExternalBufferInfo> overlay_infos;
    
    for (int i = 0; i < overlay_count; i++) {
        BufferPool::ExternalBufferInfo info;
        info.virt_addr = nullptr;        // 不需要虚拟地址
        info.phys_addr = 0;               // 物理地址动态绑定
        info.size = 0;                    // 不需要大小
        overlay_infos.push_back(info);
    }
    
    // 创建 BufferPool
    std::string pool_name = "TacoDecoder_Overlay_" + std::to_string((uintptr_t)this);
    overlay_pool_ = BufferPool::CreateFromExternal(
        overlay_infos,
        pool_name,
        "Taco_Decoder_Overlay"
    );
    
    if (!overlay_pool_) {
        printf("❌ ERROR: Failed to create overlay BufferPool\n");
        return false;
    }
    
    printf("✅ Overlay BufferPool created: '%s' (%d overlays)\n", 
           pool_name.c_str(), overlay_count);
    printf("   Registered in BufferPoolRegistry (accessible globally)\n");
    
    return true;
}

bool TacoHardwareDecoder::createAVFramePool(int overlay_count) {
    std::lock_guard<std::mutex> lock(avframe_pool_mutex_);
    
    printf("🎬 Creating AVFrame pool for %d overlays...\n", overlay_count);
    
    for (int i = 0; i < overlay_count; i++) {
        AVFrame* frame = av_frame_alloc();
        if (!frame) {
            printf("❌ ERROR: Failed to allocate AVFrame[%d]\n", i);
            cleanupAVFramePool();
            return false;
        }
        
        avframe_pool_[i] = frame;
        printf("   ✅ AVFrame[%d] allocated at %p\n", i, frame);
    }
    
    printf("✅ AVFrame pool created: %d AVFrames ready for reuse\n", overlay_count);
    
    return true;
}

std::string TacoHardwareDecoder::getOverlayPoolName() const {
    if (overlay_pool_) {
        return overlay_pool_->getName();
    }
    return "";
}

DecoderStatus TacoHardwareDecoder::receiveFrameToOverlay(
    uint32_t overlay_id, 
    DecodedFrame& out_frame) {
    
    std::lock_guard<std::mutex> lock(avframe_pool_mutex_);
    
    // 1. 检查 overlay_id 是否有效
    if (avframe_pool_.count(overlay_id) == 0) {
        printf("❌ ERROR: Invalid overlay_id %u (valid range: 0-%d)\n", 
               overlay_id, overlay_count_ - 1);
        return DecoderStatus::DECODE_ERROR;
    }
    
    // 2. 获取对应的 AVFrame（预分配的，复用）
    AVFrame* target_frame = avframe_pool_[overlay_id];
    
    // 3. 清空 AVFrame 的旧数据（如果有）
    av_frame_unref(target_frame);
    
    // 4. 调用 FFmpeg 解码到这个 AVFrame
    // 注意：这里需要访问父类 FFmpegDecoder 的 codec_ctx_
    // 由于是 protected，子类可以访问
    int ret = avcodec_receive_frame(codec_ctx_, target_frame);
    
    if (ret == AVERROR(EAGAIN)) {
        return DecoderStatus::NEED_MORE_DATA;
    } else if (ret == AVERROR_EOF) {
        return DecoderStatus::END_OF_STREAM;
    } else if (ret < 0) {
        printf("❌ ERROR: avcodec_receive_frame failed: %d\n", ret);
        return DecoderStatus::DECODE_ERROR;
    }
    
    // 5. 包装为 DecodedFrame（注意：不拥有 AVFrame 所有权）
    out_frame.av_frame = target_frame;
    out_frame.buffer = nullptr;
    out_frame.owns_av_frame = false;  // ← 关键：不拥有所有权，由 Decoder 管理
    
    printf("   ✅ Decoded to overlay %u (AVFrame[%u] at %p, format=%d, size=%dx%d)\n", 
           overlay_id, overlay_id, target_frame, 
           target_frame->format, target_frame->width, target_frame->height);
    
    return DecoderStatus::OK;
}

bool TacoHardwareDecoder::extractPhysicalAddress(
    const DecodedFrame& frame, uint64_t& out_phys_addr) {
    
    if (!frame.av_frame || !frame.av_frame->metadata) {
        printf("❌ ERROR: AVFrame or metadata is NULL\n");
        return false;
    }
    
    // 从 metadata 读取 pool_blk_id
    AVDictionaryEntry* entry = av_dict_get(
        frame.av_frame->metadata, "pool_blk_id", nullptr, 0);
    
    if (!entry) {
        printf("❌ ERROR: 'pool_blk_id' not found in AVFrame metadata\n");
        
        // 调试：打印所有 metadata 字段
        printf("🔍 DEBUG: Available metadata fields:\n");
        AVDictionaryEntry *tag = nullptr;
        while ((tag = av_dict_get(frame.av_frame->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
            printf("   - %s = %s\n", tag->key, tag->value);
        }
        return false;
    }
    
    // 解析 block_id
    uint32_t blk_id = (uint32_t)atoi(entry->value);
    if (blk_id == 0) {
        printf("❌ ERROR: Invalid blk_id=%u (parsed from pool_blk_id='%s')\n", 
               blk_id, entry->value);
        return false;
    }
    
    printf("   📦 Extracted blk_id=%u from AVFrame metadata\n", blk_id);
    
    // 使用 taco_sys 接口获取物理地址
    out_phys_addr = taco_sys_handle2_phys_addr(blk_id);
    if (out_phys_addr == 0) {
        printf("❌ ERROR: taco_sys_handle2_phys_addr failed for blk_id=%u\n", blk_id);
        return false;
    }
    
    printf("   ✅ Physical address: 0x%lx (from blk_id=%u)\n", out_phys_addr, blk_id);
    
    return true;
}

void TacoHardwareDecoder::cleanupOverlayFrame(uint32_t overlay_id) {
    std::lock_guard<std::mutex> lock(avframe_pool_mutex_);
    
    auto it = avframe_pool_.find(overlay_id);
    if (it != avframe_pool_.end()) {
        AVFrame* frame = it->second;
        if (frame) {
            // 只 unref 清空数据，不释放 AVFrame 本身（准备复用）
            av_frame_unref(frame);
            printf("   🔄 AVFrame[%u] cleaned and ready for reuse\n", overlay_id);
        }
    }
}

void TacoHardwareDecoder::cleanupAVFramePool() {
    std::lock_guard<std::mutex> lock(avframe_pool_mutex_);
    
    if (avframe_pool_.empty()) {
        return;
    }
    
    printf("🧹 Cleaning up AVFrame pool...\n");
    
    for (auto& pair : avframe_pool_) {
        if (pair.second) {
            av_frame_free(&pair.second);
            printf("   ✅ AVFrame[%u] freed\n", pair.first);
        }
    }
    
    avframe_pool_.clear();
    printf("✅ AVFrame pool cleanup complete\n");
}

