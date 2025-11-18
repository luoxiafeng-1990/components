#include "../../../include/productionline/worker/BufferFillingWorker.hpp"
#include <stdio.h>

// ============ 构造/析构 ============

BufferFillingWorker::BufferFillingWorker(BufferFillingWorkerFactory::WorkerType type)
    : preferred_type_(type), navigator_(nullptr)
{
    if (!worker_) {
        worker_ = BufferFillingWorkerFactory::create(preferred_type_);
        // 尝试将worker_转换为IVideoFileNavigator接口
        if (worker_) {
            navigator_ = dynamic_cast<IVideoFileNavigator*>(worker_.get());
        }
    }
}

BufferFillingWorker::~BufferFillingWorker() {
    // worker_ 会自动调用析构函数（智能指针）
}

// ============ 读取器类型控制 ============

void BufferFillingWorker::setWorkerType(BufferFillingWorkerFactory::WorkerType type) {
    if (navigator_ && navigator_->isOpen()) {
        printf("⚠️  Warning: Cannot change reader type while file is open\n");
        return;
    }
    
    preferred_type_ = type;
    worker_.reset();  // 清除旧的 reader
    navigator_ = nullptr;  // 清除navigator_指针
}

const char* BufferFillingWorker::getWorkerType() const {
    if (worker_) {
        // Reader 已创建：返回实际 Reader 的类型
        return worker_->getWorkerType();
    }
    // Reader 未创建：返回用户设置的偏好类型
    return BufferFillingWorkerFactory::typeToString(preferred_type_);
}

// ============ 文件操作（门面转发） ============

bool BufferFillingWorker::open(const char* path, int width, int height, int bits_per_pixel) {
    // 创建 worker（如果还没创建）
    if (!worker_) {
        worker_ = BufferFillingWorkerFactory::create(preferred_type_);
        // 尝试将worker_转换为IVideoFileNavigator接口
        if (worker_) {
            navigator_ = dynamic_cast<IVideoFileNavigator*>(worker_.get());
        }
    }
    
    // 检查是否支持文件操作
    if (!navigator_) {
        printf("❌ ERROR: Worker does not support file operations\n");
        return false;
    }
    
    // 🎯 智能判断：根据Worker类型选择合适的open方法
    // - Raw视频Worker（MMAP_RAW, IOURING_RAW）：需要格式参数，调用 openRaw()
    // - 编码视频Worker（FFMPEG_VIDEO_FILE, FFMPEG_RTSP）：自动检测格式，调用 open()
    
    bool is_raw_worker = (preferred_type_ == BufferFillingWorkerFactory::WorkerType::MMAP_RAW ||
                          preferred_type_ == BufferFillingWorkerFactory::WorkerType::IOURING_RAW);
    
    if (is_raw_worker) {
        // Raw视频Worker：使用传入的格式参数
        if (width == 0 || height == 0 || bits_per_pixel == 0) {
            printf("❌ ERROR: Raw video worker requires width, height, and bits_per_pixel!\n");
            printf("   Usage: worker.open(path, width, height, bits_per_pixel)\n");
            return false;
        }
        printf("🎬 BufferFillingWorker: Opening raw video with format %dx%d@%dbpp\n",
               width, height, bits_per_pixel);
        return navigator_->openRaw(path, width, height, bits_per_pixel);
    } else {
        // 编码视频Worker：自动检测格式（忽略 width/height/bpp 参数）
        printf("🎬 BufferFillingWorker: Opening encoded video (auto-detect format)\n");
        if (width != 0 || height != 0 || bits_per_pixel != 0) {
            printf("   Note: width/height/bpp parameters are ignored for encoded video\n");
        }
        return navigator_->open(path);
    }
}

bool BufferFillingWorker::openRaw(const char* path, int width, int height, int bits_per_pixel) {
    // 向后兼容接口：直接转发到统一的 open() 方法
    return open(path, width, height, bits_per_pixel);
}

void BufferFillingWorker::close() {
    if (navigator_) {
        navigator_->close();
    }
}

bool BufferFillingWorker::isOpen() const {
    return navigator_ && navigator_->isOpen();
}

// ============ 核心功能：填充Buffer ============

bool BufferFillingWorker::fillBuffer(int frame_index, Buffer* buffer) {
    if (!worker_) {
        printf("❌ ERROR: Worker not initialized\n");
        return false;
    }
    return worker_->fillBuffer(frame_index, buffer);
}

// ============ 导航操作（门面转发） ============

bool BufferFillingWorker::seek(int frame_index) {
    if (!navigator_) {
        printf("❌ ERROR: Navigator not initialized\n");
        return false;
    }
    return navigator_->seek(frame_index);
}

bool BufferFillingWorker::seekToBegin() {
    if (!navigator_) {
        printf("❌ ERROR: Navigator not initialized\n");
        return false;
    }
    return navigator_->seekToBegin();
}

bool BufferFillingWorker::seekToEnd() {
    if (!navigator_) {
        printf("❌ ERROR: Navigator not initialized\n");
        return false;
    }
    return navigator_->seekToEnd();
}

bool BufferFillingWorker::skip(int frame_count) {
    if (!navigator_) {
        printf("❌ ERROR: Navigator not initialized\n");
        return false;
    }
    return navigator_->skip(frame_count);
}

// ============ 信息查询（门面转发） ============

int BufferFillingWorker::getTotalFrames() const {
    return navigator_ ? navigator_->getTotalFrames() : 0;
}

int BufferFillingWorker::getCurrentFrameIndex() const {
    return navigator_ ? navigator_->getCurrentFrameIndex() : 0;
}

size_t BufferFillingWorker::getFrameSize() const {
    return navigator_ ? navigator_->getFrameSize() : 0;
}

long BufferFillingWorker::getFileSize() const {
    return navigator_ ? navigator_->getFileSize() : 0;
}

int BufferFillingWorker::getWidth() const {
    return navigator_ ? navigator_->getWidth() : 0;
}

int BufferFillingWorker::getHeight() const {
    return navigator_ ? navigator_->getHeight() : 0;
}

int BufferFillingWorker::getBytesPerPixel() const {
    return navigator_ ? navigator_->getBytesPerPixel() : 0;
}

const char* BufferFillingWorker::getPath() const {
    return navigator_ ? navigator_->getPath() : "";
}

bool BufferFillingWorker::hasMoreFrames() const {
    return navigator_ && navigator_->hasMoreFrames();
}

bool BufferFillingWorker::isAtEnd() const {
    return navigator_ && navigator_->isAtEnd();
}

bool BufferFillingWorker::requiresExternalBuffer() const {
    if (worker_) {
        return worker_->requiresExternalBuffer();
    }
    // 默认保守：假设需要外部 buffer
    return true;
}

// ============ 提供原材料（BufferPool）============

std::unique_ptr<BufferPool> BufferFillingWorker::getOutputBufferPool() {
    if (worker_) {
        return worker_->getOutputBufferPool();
    }
    return nullptr;
}

void* BufferFillingWorker::getOutputBufferPoolRaw() const {
    if (worker_) {
        return worker_->getOutputBufferPoolRaw();
    }
    return nullptr;
}

