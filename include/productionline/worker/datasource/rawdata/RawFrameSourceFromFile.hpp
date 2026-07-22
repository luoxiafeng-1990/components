#ifndef RAW_FRAME_SOURCE_FROM_FILE_HPP
#define RAW_FRAME_SOURCE_FROM_FILE_HPP

#include "productionline/worker/datasource/rawdata/IRawFrameSource.hpp"
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include <string>
#include <cstdio>
#include <atomic>

extern "C" {
#include <libavutil/pixfmt.h>
}

/**
 * @brief RawFrameSourceFromFile - 从 YUV/RGB 文件读取原始帧数据
 * 
 * 功能：从原始视频文件读取帧数据（支持 NV12、NV21、YUV420P、RGB 等格式）
 * 
 * 使用场景：
 * - 编码测试（从 YUV 文件读取帧进行编码）
 * - 视频处理（读取原始帧进行处理）
 * - 质量验证（读取参考帧进行比较）
 * 
 * loop_count：同一文件循环读取遍数。有效总帧数 = 文件帧数 × loop_count。
 * 达到有效总帧数后返回 EOF；EncodeWorker 仅消费数据源，不感知循环。
 * 
 * 命名规范：
 * - 遵循 EncodedPacketSourceFromFile 的命名模式
 * - RawFrame = 原始帧（未编码的 YUV/RGB 数据）
 * - FromFile = 数据来源是文件
 * 
 * 注意：
 * - 需要预先知道帧的宽度、高度和像素格式
 * - YUV 文件是裸数据，没有头信息
 */
class RawFrameSourceFromFile : public IRawFrameSource {
public:
    /**
     * @brief 构造函数
     * @param file_path YUV/RGB 文件路径
     * @param width 帧宽度（像素）
     * @param height 帧高度（像素）
     * @param pix_fmt 像素格式（默认 NV12）
     * @param loop_count 文件循环遍数（默认 1；<1 按 1 处理）
     */
    RawFrameSourceFromFile(const std::string& file_path,
                           int width,
                           int height,
                           AVPixelFormat pix_fmt = AV_PIX_FMT_NV12,
                           int loop_count = 1);
    
    /**
     * @brief 析构函数
     */
    ~RawFrameSourceFromFile() override;
    
    // 禁止拷贝
    RawFrameSourceFromFile(const RawFrameSourceFromFile&) = delete;
    RawFrameSourceFromFile& operator=(const RawFrameSourceFromFile&) = delete;
    
    // ============ IRawFrameSource 接口实现 ============
    int readRawFrame(AVFrame* frame) override;
    int readRawFrameByPts(int64_t pts, AVFrame* frame) override;
    int getFrameWidth() const override { return width_; }
    int getFrameHeight() const override { return height_; }
    
    // ============ IDataSourceNavigator 接口实现 ============
    
    // 数据源生命周期
    bool open() override;
    bool open(const char* path) override;
    void close() override;
    bool isOpen() const override;
    
    // 数据源导航（按文件内帧索引 0..file_frames_-1）
    bool seek(int frame_index) override;
    bool seekToBegin() override;
    bool seekToEnd() override;
    bool skip(int frame_count) override;
    
    // 数据源状态查询
    int getTotalFrames() const override;
    int getCurrentFrameIndex() const override;
    size_t getFrameSize() const override;
    long getFileSize() const override;
    std::string getPath() const override;
    bool hasMoreFrames() const override;
    bool isAtEnd() const override;
    
    // 数据源属性
    int getSourceWidth() const override { return width_; }
    int getSourceHeight() const override { return height_; }
    AVPixelFormat getSourcePixelFormat() const override { return pix_fmt_; }
    const AVCodecParameters* getCodecParameters() const override { return nullptr; }
    SourceType getDataSourceType() const override { return SourceType::FILE_SOURCE; }

private:
    std::string file_path_;          // 文件路径
    int width_;                      // 帧宽度
    int height_;                     // 帧高度
    AVPixelFormat pix_fmt_;          // 像素格式
    FILE* file_ptr_;                 // 文件指针
    int current_frame_index_;        // 当前文件内帧索引
    int file_frames_;                // 文件内真实帧数
    int total_frames_;               // 有效总帧数 = file_frames_ * loop_count_
    int loop_count_;                 // 循环遍数（>=1）
    int frames_delivered_;           // 已成功交付帧数（跨遍累计）
    size_t frame_size_;              // 单帧大小（字节）
    std::atomic<bool> is_open_;      // 打开状态
    bool eof_reached_;               // 是否到达有效末尾
    
    /**
     * @brief 计算单帧大小（根据像素格式）
     */
    size_t calculateFrameSize() const;

    /**
     * @brief 从当前位置读取一帧到 frame（不处理 loop 回绕）
     * @return 0=成功, AVERROR_EOF=本遍结束/不完整, <0=错误
     */
    int readOneFrameFromFile(AVFrame* frame);
    
    // 日志器
    log4cplus::Logger logger_;
};

#endif // RAW_FRAME_SOURCE_FROM_FILE_HPP
