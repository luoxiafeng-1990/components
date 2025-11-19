#include "productionline/worker/facade/BufferFillingWorkerFacade.hpp"
#include <stdio.h>

// ============ 构造/析构 ============

BufferFillingWorkerFacade::BufferFillingWorkerFacade(BufferFillingWorkerFactory::WorkerType type)
    : preferred_type_(type)
{
    if (!worker_) {
        worker_ = BufferFillingWorkerFactory::create(preferred_type_);
    }
}

BufferFillingWorkerFacade::~BufferFillingWorkerFacade() {
    // worker_ 会自动调用析构函数（智能指针）
}

// ============ Worker类型控制 ============

void BufferFillingWorkerFacade::setWorkerType(BufferFillingWorkerFactory::WorkerType type) {
    if (worker_ && worker_->isOpen()) {
        printf("⚠️  Warning: Cannot change worker type while file is open\n");
        return;
    }
    
    preferred_type_ = type;
    worker_.reset();  // 清除旧的 worker
}

const char* BufferFillingWorkerFacade::getWorkerType() const {
    if (worker_) {
        // Worker 已创建：返回实际 Worker 的类型
        return worker_->getWorkerType();
    }
    // Worker 未创建：返回用户设置的偏好类型
    return BufferFillingWorkerFactory::typeToString(preferred_type_);
}

// ============ 文件操作（门面转发） ============

bool BufferFillingWorkerFacade::open(const char* path) {
    // 创建 worker（如果还没创建）
    if (!worker_) {
        worker_ = BufferFillingWorkerFactory::create(preferred_type_);
    }
    
    if (!worker_) {
        printf("❌ ERROR: Failed to create worker\n");
        return false;
    }
    
    // 编码视频Worker：自动检测格式
    printf("🎬 BufferFillingWorkerFacade: Opening encoded video (auto-detect format)\n");
    return worker_->open(path);
}

bool BufferFillingWorkerFacade::open(const char* path, int width, int height, int bits_per_pixel) {
    // 创建 worker（如果还没创建）
    if (!worker_) {
        worker_ = BufferFillingWorkerFactory::create(preferred_type_);
    }
    
    if (!worker_) {
        printf("❌ ERROR: Failed to create worker\n");
        return false;
    }
    
    // 🎯 智能判断：根据Worker类型选择合适的open方法
    // - Raw视频Worker（MMAP_RAW, IOURING_RAW）：需要格式参数，调用 open(path, width, height, bits_per_pixel)
    // - 编码视频Worker（FFMPEG_VIDEO_FILE, FFMPEG_RTSP）：自动检测格式，调用 open(path)
    
    bool is_raw_worker = (preferred_type_ == BufferFillingWorkerFactory::WorkerType::MMAP_RAW ||
                          preferred_type_ == BufferFillingWorkerFactory::WorkerType::IOURING_RAW);
    
    if (is_raw_worker) {
        // Raw视频Worker：使用传入的格式参数
        if (width == 0 || height == 0 || bits_per_pixel == 0) {
            printf("❌ ERROR: Raw video worker requires width, height, and bits_per_pixel!\n");
            printf("   Usage: worker.open(path, width, height, bits_per_pixel)\n");
            return false;
        }
        printf("🎬 BufferFillingWorkerFacade: Opening raw video with format %dx%d@%dbpp\n",
               width, height, bits_per_pixel);
        return worker_->open(path, width, height, bits_per_pixel);
    } else {
        // 编码视频Worker：自动检测格式（忽略 width/height/bpp 参数）
        printf("🎬 BufferFillingWorkerFacade: Opening encoded video (auto-detect format)\n");
        if (width != 0 || height != 0 || bits_per_pixel != 0) {
            printf("   Note: width/height/bpp parameters are ignored for encoded video\n");
        }
        return worker_->open(path);
    }
}

void BufferFillingWorkerFacade::close() {
    if (worker_) {
        worker_->close();
    }
}

bool BufferFillingWorkerFacade::isOpen() const {
    return worker_ && worker_->isOpen();
}

// ============ 核心功能：填充Buffer ============

bool BufferFillingWorkerFacade::fillBuffer(int frame_index, Buffer* buffer) {
    if (!worker_) {
        printf("❌ ERROR: Worker not initialized\n");
        return false;
    }
    return worker_->fillBuffer(frame_index, buffer);
}

// ============ 导航操作（门面转发） ============

bool BufferFillingWorkerFacade::seek(int frame_index) {
    if (!worker_) {
        printf("❌ ERROR: Worker not initialized\n");
        return false;
    }
    return worker_->seek(frame_index);
}

bool BufferFillingWorkerFacade::seekToBegin() {
    if (!worker_) {
        printf("❌ ERROR: Worker not initialized\n");
        return false;
    }
    return worker_->seekToBegin();
}

bool BufferFillingWorkerFacade::seekToEnd() {
    if (!worker_) {
        printf("❌ ERROR: Worker not initialized\n");
        return false;
    }
    return worker_->seekToEnd();
}

bool BufferFillingWorkerFacade::skip(int frame_count) {
    if (!worker_) {
        printf("❌ ERROR: Worker not initialized\n");
        return false;
    }
    return worker_->skip(frame_count);
}

// ============ 信息查询（门面转发） ============

int BufferFillingWorkerFacade::getTotalFrames() const {
    return worker_ ? worker_->getTotalFrames() : 0;
}

int BufferFillingWorkerFacade::getCurrentFrameIndex() const {
    return worker_ ? worker_->getCurrentFrameIndex() : 0;
}

size_t BufferFillingWorkerFacade::getFrameSize() const {
    return worker_ ? worker_->getFrameSize() : 0;
}

long BufferFillingWorkerFacade::getFileSize() const {
    return worker_ ? worker_->getFileSize() : 0;
}

int BufferFillingWorkerFacade::getWidth() const {
    return worker_ ? worker_->getWidth() : 0;
}

int BufferFillingWorkerFacade::getHeight() const {
    return worker_ ? worker_->getHeight() : 0;
}

int BufferFillingWorkerFacade::getBytesPerPixel() const {
    return worker_ ? worker_->getBytesPerPixel() : 0;
}

const char* BufferFillingWorkerFacade::getPath() const {
    return worker_ ? worker_->getPath() : "";
}

bool BufferFillingWorkerFacade::hasMoreFrames() const {
    return worker_ && worker_->hasMoreFrames();
}

bool BufferFillingWorkerFacade::isAtEnd() const {
    return worker_ && worker_->isAtEnd();
}

// ============ 提供原材料（BufferPool）============

std::unique_ptr<BufferPool> BufferFillingWorkerFacade::getOutputBufferPool() {
    if (worker_) {
        return worker_->getOutputBufferPool();
    }
    return nullptr;
}

