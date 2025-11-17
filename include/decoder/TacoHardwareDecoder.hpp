#ifndef TACO_HARDWARE_DECODER_HPP
#define TACO_HARDWARE_DECODER_HPP

#include "FFmpegDecoder.hpp"
#include "../buffer/BufferPool.hpp"
#include <map>
#include <mutex>
#include <memory>

// 前向声明 taco_sys 接口
extern "C" {
    uint64_t taco_sys_handle2_phys_addr(uint32_t handle);
}

/**
 * TacoHardwareDecoder - Taco 硬件解码器（h264_taco 专用）
 * 
 * 职责：
 * - 封装 h264_taco 硬件解码器
 * - 管理 overlay BufferPool（注册到全局注册表）
 * - 管理 AVFrame 池（每个 overlay ID 对应一个 AVFrame）
 * - 提供物理地址提取接口（用于 DMA 零拷贝）
 * 
 * 特性：
 * - 零拷贝解码（FFmpeg 直接解码到 taco_sys 内存）
 * - 物理地址导出（用于 framebuffer DMA）
 * - AVFrame 池复用（避免频繁分配/释放）
 * - 双通道输出（CH0: YUV, CH1: RGB）
 * 
 * 使用场景：
 * - 嵌入式硬件平台（Taco FPGA）
 * - 需要 DMA 零拷贝显示
 * - 高性能视频播放
 * 
 * 架构：
 * ```
 * TacoHardwareDecoder
 * ├─ FFmpeg (h264_taco codec)
 * ├─ BufferPool (overlay ID 池，注册到 BufferPoolRegistry)
 * │   ├─ Buffer(id=0) ←→ AVFrame[0]
 * │   ├─ Buffer(id=1) ←→ AVFrame[1]
 * │   ├─ Buffer(id=2) ←→ AVFrame[2]
 * │   └─ Buffer(id=3) ←→ AVFrame[3]
 * └─ AVFrame 池 (预分配，生命周期由 Decoder 管理)
 * ```
 */
class TacoHardwareDecoder : public FFmpegDecoder {
public:
    TacoHardwareDecoder();
    ~TacoHardwareDecoder() override;
    
    // 禁用拷贝
    TacoHardwareDecoder(const TacoHardwareDecoder&) = delete;
    TacoHardwareDecoder& operator=(const TacoHardwareDecoder&) = delete;
    
    // ============ 初始化（扩展：创建并注册 BufferPool）============
    
    /**
     * @brief 初始化解码器 + 创建 overlay BufferPool + AVFrame 池
     * @param config 解码器配置
     * @param overlay_count overlay 数量（1-4）
     * @return DecoderStatus
     * 
     * 内部流程：
     * 1. 调用父类 initialize（初始化 FFmpeg 解码器）
     * 2. 创建 overlay BufferPool（管理 overlay ID）
     * 3. 为每个 overlay ID 预分配一个 AVFrame
     * 4. 建立 overlay ID ←→ AVFrame 的映射关系
     * 5. 注册 BufferPool 到 BufferPoolRegistry
     */
    DecoderStatus initializeWithOverlayPool(
        const DecoderConfig& config, 
        int overlay_count);
    
    /**
     * @brief 解码到指定的 overlay
     * @param overlay_id overlay ID（对应 Buffer->id()）
     * @param[out] out_frame 输出解码后的 DecodedFrame
     * @return DecoderStatus
     * 
     * 工作流程：
     * 1. 根据 overlay_id 获取对应的 AVFrame
     * 2. 清空 AVFrame 的旧数据（av_frame_unref）
     * 3. 调用 avcodec_receive_frame(codec_ctx, avframe_pool_[overlay_id])
     * 4. 解码数据直接写入这个 AVFrame（复用）
     * 5. 返回 DecodedFrame（包含 AVFrame 引用，但不拥有所有权）
     * 
     * 注意：
     * - AVFrame 由 Decoder 管理，调用者不需要释放
     * - AVFrame 会在 overlay 归还时自动复用
     * - out_frame.owns_av_frame = false（不拥有所有权）
     */
    DecoderStatus receiveFrameToOverlay(
        uint32_t overlay_id, 
        DecodedFrame& out_frame);
    
    /**
     * @brief 从解码帧提取物理地址
     * @param frame 解码后的 DecodedFrame
     * @param[out] out_phys_addr 输出物理地址
     * @return true 成功，false 失败
     * 
     * 工作流程：
     * 1. 从 AVFrame metadata 读取 "pool_blk_id"
     * 2. 调用 taco_sys_handle2_phys_addr(blk_id) 获取物理地址
     * 3. 返回物理地址供 DMA 使用
     */
    bool extractPhysicalAddress(const DecodedFrame& frame, uint64_t& out_phys_addr);
    
    /**
     * @brief 配置 Taco 双通道输出（在 initialize 之前调用）
     * @param ch0_enable 通道0使能（YUV输出）
     * @param ch1_enable 通道1使能（RGB输出）
     * @param ch1_rgb_format RGB格式（如 "argb888", "rgb888_planar"）
     * @param ch1_rgb_std 颜色标准（如 "bt601", "bt709"）
     * @return DecoderStatus
     */
    DecoderStatus configureDualChannel(
        bool ch0_enable,
        bool ch1_enable,
        const char* ch1_rgb_format = "argb888",
        const char* ch1_rgb_std = "bt601");
    
    /**
     * @brief 获取 overlay BufferPool
     */
    BufferPool* getOverlayPool() const { return overlay_pool_.get(); }
    
    /**
     * @brief 获取 BufferPool 名称
     */
    std::string getOverlayPoolName() const;
    
    /**
     * @brief 清理 overlay 对应的 AVFrame（当 overlay 归还时调用）
     * 
     * 注意：这里不是释放 AVFrame，只是 unref 清空数据，准备复用
     */
    void cleanupOverlayFrame(uint32_t overlay_id);
    
    /**
     * @brief 检查是否支持物理地址导出
     */
    bool supportsPhysicalAddressExport() const { return true; }
    
    const char* getDecoderType() const override { 
        return "TacoHardwareDecoder"; 
    }
    
private:
    // ============ Overlay 管理 ============
    
    std::unique_ptr<BufferPool> overlay_pool_;   // overlay ID 池
    int overlay_count_;                          // overlay 数量
    
    // ============ AVFrame 池（核心！）============
    
    /**
     * AVFrame 池：每个 overlay ID 对应一个 AVFrame
     * - key: overlay ID (0-3)
     * - value: AVFrame* (预分配，生命周期由 Decoder 管理)
     */
    std::map<uint32_t, AVFrame*> avframe_pool_;
    std::mutex avframe_pool_mutex_;
    
    // ============ Taco 双通道配置 ============
    
    bool ch0_enabled_;
    bool ch1_enabled_;
    std::string ch1_rgb_format_;
    std::string ch1_rgb_std_;
    
    // ============ 内部辅助函数 ============
    
    /**
     * @brief 创建 overlay BufferPool
     */
    bool createOverlayPool(int overlay_count);
    
    /**
     * @brief 创建 AVFrame 池（为每个 overlay 预分配 AVFrame）
     */
    bool createAVFramePool(int overlay_count);
    
    /**
     * @brief 清理 AVFrame 池
     */
    void cleanupAVFramePool();
    
    /**
     * @brief 配置 h264_taco 解码器选项
     */
    DecoderStatus configureTacoOptions();
};

#endif // TACO_HARDWARE_DECODER_HPP

