#pragma once

#include <cstdint>
#include <cstring>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/frame.h>
}

// 前向声明
class Buffer;
namespace cv { class Mat; }

/**
 * @brief 图像帧元数据（只读值对象，独立于 Buffer）
 * 
 * 消费者在需要图像元数据时，自行从载荷（AVFrame/Mat）中提取。
 * Buffer 本身不持有图像描述信息 — 保持纯粹的"内存容器 + 时间戳"职责。
 * 
 * 命名参考：GStreamer GstVideoMeta / Android MediaFormat
 * 
 * 设计原则：
 *   1. 仅通过工厂方法构造，字段只读
 *   2. 值语义，可自由复制和传递
 *   3. 不持有资源，不管理生命周期
 * 
 * 用法：
 *   auto meta = ImageMeta::fromBuffer(buffer);
 *   if (meta.isValid()) {
 *       int w = meta.width();
 *       const uint8_t* y_plane = meta.planeData(0);
 *   }
 */
class ImageMeta {
public:
    // ========== 访问器 ==========
    
    int width() const { return width_; }
    int height() const { return height_; }
    AVPixelFormat format() const { return format_; }
    int nbPlanes() const { return nb_planes_; }
    
    /** @brief 获取指定 plane 的数据指针 */
    uint8_t* planeData(int i) const {
        return (i >= 0 && i < 4) ? plane_data_[i] : nullptr;
    }
    
    /** @brief 获取指定 plane 的行字节数 */
    int linesize(int i) const {
        return (i >= 0 && i < 4) ? linesize_[i] : 0;
    }
    
    /** @brief 获取指定 plane 的偏移量 */
    size_t planeOffset(int i) const {
        return (i >= 0 && i < 4) ? plane_offset_[i] : 0;
    }
    
    /** @brief 检查元数据是否有效（AVFrame 或 Mat 均可） */
    bool isValid() const {
        return width_ > 0 && height_ > 0 &&
               (format_ != AV_PIX_FMT_NONE || plane_data_[0] != nullptr);
    }
    
    // ========== 工厂方法 ==========
    
    /** @brief 从 AVFrame 提取图像元数据 */
    static ImageMeta fromAVFrame(const AVFrame* frame);
    
    /** @brief 从 Buffer 中提取图像元数据（按载荷类型自动选择数据源） */
    static ImageMeta fromBuffer(const Buffer* buffer);
    
    /** @brief 从 cv::Mat 提取图像元数据（用于 BufferComparator 等无需 Buffer 包装的场景） */
    static ImageMeta fromMat(const cv::Mat* mat);

private:
    int width_ = 0;
    int height_ = 0;
    AVPixelFormat format_ = AV_PIX_FMT_NONE;
    int linesize_[4] = {};
    size_t plane_offset_[4] = {};
    int nb_planes_ = 0;
    uint8_t* plane_data_[4] = {};
};
