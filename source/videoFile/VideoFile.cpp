#include "../../include/videoFile/VideoFile.hpp"
#include <stdio.h>

// ============ 构造/析构 ============

VideoFile::VideoFile(VideoReaderFactory::ReaderType type)
    : preferred_type_(type)
{
    // 延迟创建 reader_，在 open/openRaw 时创建
    // 这样可以避免未使用时的资源浪费
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
        return reader_->getReaderType();
    }
    return "None (not initialized)";
}

// ============ 文件操作（门面转发） ============

bool VideoFile::open(const char* path) {
    // 创建 reader（如果还没创建）
    if (!reader_) {
        reader_ = VideoReaderFactory::create(preferred_type_);
    }
    
    return reader_->open(path);
}

bool VideoFile::openRaw(const char* path, int width, int height, int bits_per_pixel) {
    // 创建 reader（如果还没创建）
    if (!reader_) {
        reader_ = VideoReaderFactory::create(preferred_type_);
    }
    
    return reader_->openRaw(path, width, height, bits_per_pixel);
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

// ============ 可选依赖注入（转发） ============

void VideoFile::setBufferPool(void* pool) {
    if (reader_) {
        reader_->setBufferPool(pool);
    }
}

