#include "../../include/videoFile/VideoFile.hpp"
#include <stdio.h>

// ============ 构造/析构 ============

VideoFile::VideoFile(VideoReaderFactory::ReaderType type)
    : preferred_type_(type)
{
    if (!reader_) {
        reader_ = VideoReaderFactory::create(preferred_type_);
    }
}

VideoFile::~VideoFile() {
    // reader_ 会自动调用析构函数（智能指针）
}

// ============ 读取器类型控制 ============

void VideoFile::setReaderType(VideoReaderFactory::ReaderType type) {
    if (reader_ && reader_->isOpen()) {
        printf("⚠️  Warning: Cannot change reader type while file is open\n");
        return;
    }
    
    preferred_type_ = type;
    reader_.reset();  // 清除旧的 reader
}

const char* VideoFile::getReaderType() const {
    if (reader_) {
        // Reader 已创建：返回实际 Reader 的类型
        return reader_->getReaderType();
    }
    // Reader 未创建：返回用户设置的偏好类型
    return VideoReaderFactory::typeToString(preferred_type_);
}

// ============ 文件操作（门面转发） ============

bool VideoFile::open(const char* path, int width, int height, int bits_per_pixel) {
    // 创建 reader（如果还没创建）
    if (!reader_) {
        reader_ = VideoReaderFactory::create(preferred_type_);
    }
    
    // 🎯 智能判断：根据Reader类型选择合适的open方法
    // - Raw视频Reader（MMAP, IOURING, DIRECT_READ）：需要格式参数，调用 openRaw()
    // - 编码视频Reader（FFMPEG, RTSP）：自动检测格式，调用 open()
    
    bool is_raw_reader = (preferred_type_ == VideoReaderFactory::ReaderType::MMAP ||
                          preferred_type_ == VideoReaderFactory::ReaderType::IOURING ||
                          preferred_type_ == VideoReaderFactory::ReaderType::DIRECT_READ);
    
    if (is_raw_reader) {
        // Raw视频Reader：使用传入的格式参数
        if (width == 0 || height == 0 || bits_per_pixel == 0) {
            printf("❌ ERROR: Raw video reader requires width, height, and bits_per_pixel!\n");
            printf("   Usage: video.open(path, width, height, bits_per_pixel)\n");
            return false;
        }
        printf("🎬 VideoFile: Opening raw video with format %dx%d@%dbpp\n",
               width, height, bits_per_pixel);
        return reader_->openRaw(path, width, height, bits_per_pixel);
    } else {
        // 编码视频Reader：自动检测格式（忽略 width/height/bpp 参数）
        printf("🎬 VideoFile: Opening encoded video (auto-detect format)\n");
        if (width != 0 || height != 0 || bits_per_pixel != 0) {
            printf("   Note: width/height/bpp parameters are ignored for encoded video\n");
        }
        return reader_->open(path);
    }
}

bool VideoFile::openRaw(const char* path, int width, int height, int bits_per_pixel) {
    // 向后兼容接口：直接转发到统一的 open() 方法
    return open(path, width, height, bits_per_pixel);
}

void VideoFile::close() {
    if (reader_) {
        reader_->close();
    }
}

bool VideoFile::isOpen() const {
    return reader_ && reader_->isOpen();
}

// ============ 读取操作（门面转发） ============

bool VideoFile::readFrameTo(Buffer& dest_buffer) {
    if (!reader_) {
        printf("❌ ERROR: Reader not initialized\n");
        return false;
    }
    return reader_->readFrameTo(dest_buffer);
}

bool VideoFile::readFrameTo(void* dest_buffer, size_t buffer_size) {
    if (!reader_) {
        printf("❌ ERROR: Reader not initialized\n");
        return false;
    }
    return reader_->readFrameTo(dest_buffer, buffer_size);
}

bool VideoFile::readFrameAt(int frame_index, Buffer& dest_buffer) {
    if (!reader_) {
        printf("❌ ERROR: Reader not initialized\n");
        return false;
    }
    return reader_->readFrameAt(frame_index, dest_buffer);
}

bool VideoFile::readFrameAt(int frame_index, void* dest_buffer, size_t buffer_size) {
    if (!reader_) {
        printf("❌ ERROR: Reader not initialized\n");
        return false;
    }
    return reader_->readFrameAt(frame_index, dest_buffer, buffer_size);
}

bool VideoFile::readFrameAtThreadSafe(int frame_index, void* dest_buffer, size_t buffer_size) const {
    if (!reader_) {
        return false;
    }
    return reader_->readFrameAtThreadSafe(frame_index, dest_buffer, buffer_size);
}

bool VideoFile::readFrame(int frame_index, Buffer* buffer) {
    if (!reader_) {
        printf("❌ ERROR: Reader not initialized\n");
        return false;
    }
    return reader_->readFrame(frame_index, buffer);
}

// ============ 导航操作（门面转发） ============

bool VideoFile::seek(int frame_index) {
    if (!reader_) {
        printf("❌ ERROR: Reader not initialized\n");
        return false;
    }
    return reader_->seek(frame_index);
}

bool VideoFile::seekToBegin() {
    if (!reader_) {
        printf("❌ ERROR: Reader not initialized\n");
        return false;
    }
    return reader_->seekToBegin();
}

bool VideoFile::seekToEnd() {
    if (!reader_) {
        printf("❌ ERROR: Reader not initialized\n");
        return false;
    }
    return reader_->seekToEnd();
}

bool VideoFile::skip(int frame_count) {
    if (!reader_) {
        printf("❌ ERROR: Reader not initialized\n");
        return false;
    }
    return reader_->skip(frame_count);
}

// ============ 信息查询（门面转发） ============

int VideoFile::getTotalFrames() const {
    return reader_ ? reader_->getTotalFrames() : 0;
}

int VideoFile::getCurrentFrameIndex() const {
    return reader_ ? reader_->getCurrentFrameIndex() : 0;
}

size_t VideoFile::getFrameSize() const {
    return reader_ ? reader_->getFrameSize() : 0;
}

long VideoFile::getFileSize() const {
    return reader_ ? reader_->getFileSize() : 0;
}

int VideoFile::getWidth() const {
    return reader_ ? reader_->getWidth() : 0;
}

int VideoFile::getHeight() const {
    return reader_ ? reader_->getHeight() : 0;
}

int VideoFile::getBytesPerPixel() const {
    return reader_ ? reader_->getBytesPerPixel() : 0;
}

const char* VideoFile::getPath() const {
    return reader_ ? reader_->getPath() : "";
}

bool VideoFile::hasMoreFrames() const {
    return reader_ && reader_->hasMoreFrames();
}

bool VideoFile::isAtEnd() const {
    return reader_ && reader_->isAtEnd();
}

bool VideoFile::requiresExternalBuffer() const {
    if (reader_) {
        return reader_->requiresExternalBuffer();
    }
    // 默认保守：假设需要外部 buffer
    return true;
}

// ============ 可选依赖注入（转发） ============

void VideoFile::setBufferPool(void* pool) {
    if (reader_) {
        reader_->setBufferPool(pool);
    }
}

void* VideoFile::getOutputBufferPool() const {
    if (reader_) {
        return reader_->getOutputBufferPool();
    }
    return nullptr;
}

