#include "productionline/worker/BufferFillingWorkerFacade.hpp"
#include "common/Logger.hpp"
#include <stdio.h>

// ============ 构造/析构 ============

BufferFillingWorkerFacade::BufferFillingWorkerFacade(const WorkerConfig& config)
    : config_(config)
{
    if (!worker_base_uptr_) {
        worker_base_uptr_ = BufferFillingWorkerFactory::create(config_);
    }
}

BufferFillingWorkerFacade::~BufferFillingWorkerFacade() {
    // worker_base_uptr_ 会自动调用析构函数（智能指针）
}

// ============ Buffer填充方法 ============

const char* BufferFillingWorkerFacade::getWorkerType() const {
    if (worker_base_uptr_) {
        // Worker 已创建：返回实际 Worker 的类型
        return worker_base_uptr_->getWorkerType();
    }
    // Worker 未创建：返回 config_ 中设置的 worker_type
    return BufferFillingWorkerFactory::typeToString(config_.worker_type);
}

// ============ 文件操作（门面转发） ============

bool BufferFillingWorkerFacade::open() {
    // 创建 worker（如果还没创建）
    if (!worker_base_uptr_) {
        worker_base_uptr_ = BufferFillingWorkerFactory::create(config_);
    }
    
    if (!worker_base_uptr_) {
        LOG_ERROR("[Worker] ERROR: Failed to create worker");
        return false;
    }
    
    // ⭐ v2.9新增：检查是否是 Buffer 模式
    bool is_buffer_mode = config_.decoder.use_buffer_mode;
    
    if (is_buffer_mode) {
        // Buffer 模式：不需要文件路径，直接调用 open(nullptr)
        LOG_DEBUG("[Worker] BufferFillingWorkerFacade: Opening in Buffer mode (no file path needed)");
        return worker_base_uptr_->open(nullptr);
    }
    
    // 文件模式：从 config_ 获取所有参数
    const std::string& file_path = config_.file.file_path;
    int width = config_.display.width;
    int height = config_.display.height;
    int bits_per_pixel = config_.display.bits_per_pixel;
    
    if (file_path.empty()) {
        LOG_ERROR("[Worker] ERROR: File path not set in config");
        return false;
    }
    
    const char* path = file_path.c_str();
    
    // 🎯 智能判断：根据Worker类型选择合适的open方法
    // - 需要格式参数的Worker：MMAP_RAW, IOURING_RAW, FFMPEG_RTSP
    //   （原始视频文件和RTSP实时流都需要明确指定输出格式）
    // - 可以自动检测的Worker：FFMPEG_VIDEO_FILE
    //   （本地编码视频文件有文件头，可以自动检测分辨率和格式）
    
    bool needs_format_params = (config_.worker_type == BufferFillingWorkerFactory::WorkerType::MMAP_RAW ||
                                config_.worker_type == BufferFillingWorkerFactory::WorkerType::IOURING_RAW ||
                                config_.worker_type == BufferFillingWorkerFactory::WorkerType::FFMPEG_RTSP);
    
    if (needs_format_params) {
        // 需要格式参数的Worker（原始视频、RTSP流）
        if (width == 0 || height == 0 || bits_per_pixel == 0) {
            LOG_ERROR_FMT("[Worker] ERROR: Worker type '%s' requires width, height, and bits_per_pixel in config!",
                         BufferFillingWorkerFactory::typeToString(config_.worker_type));
            return false;
        }
        LOG_DEBUG_FMT("[Worker] BufferFillingWorkerFacade: Opening video with format %dx%d@%dbpp",
               width, height, bits_per_pixel);
        return worker_base_uptr_->open(path, width, height, bits_per_pixel);
    } else {
        // 可以自动检测格式的Worker（本地编码视频文件）
        LOG_DEBUG("[Worker] BufferFillingWorkerFacade: Opening encoded video file (auto-detect format)");
        return worker_base_uptr_->open(path);
    }
}

void BufferFillingWorkerFacade::close() {
    if (worker_base_uptr_) {
        worker_base_uptr_->close();
    }
}

bool BufferFillingWorkerFacade::isOpen() const {
    return worker_base_uptr_ && worker_base_uptr_->isOpen();
}

// ============ 核心功能：填充Buffer ============

bool BufferFillingWorkerFacade::fillBuffer(int frame_index, Buffer* buffer) {
    if (!worker_base_uptr_) {
        LOG_ERROR("[Worker] ERROR: Worker not initialized");
        return false;
    }
    return worker_base_uptr_->fillBuffer(frame_index, buffer);
}

// ============ 导航操作（门面转发） ============

bool BufferFillingWorkerFacade::seek(int frame_index) {
    if (!worker_base_uptr_) {
        LOG_ERROR("[Worker] ERROR: Worker not initialized");
        return false;
    }
    return worker_base_uptr_->seek(frame_index);
}

bool BufferFillingWorkerFacade::seekToBegin() {
    if (!worker_base_uptr_) {
        LOG_ERROR("[Worker] ERROR: Worker not initialized");
        return false;
    }
    return worker_base_uptr_->seekToBegin();
}

bool BufferFillingWorkerFacade::seekToEnd() {
    if (!worker_base_uptr_) {
        LOG_ERROR("[Worker] ERROR: Worker not initialized");
        return false;
    }
    return worker_base_uptr_->seekToEnd();
}

bool BufferFillingWorkerFacade::skip(int frame_count) {
    if (!worker_base_uptr_) {
        LOG_ERROR("[Worker] ERROR: Worker not initialized");
        return false;
    }
    return worker_base_uptr_->skip(frame_count);
}

// ============ 信息查询（门面转发） ============

int BufferFillingWorkerFacade::getTotalFrames() const {
    return worker_base_uptr_ ? worker_base_uptr_->getTotalFrames() : 0;
}

int BufferFillingWorkerFacade::getCurrentFrameIndex() const {
    return worker_base_uptr_ ? worker_base_uptr_->getCurrentFrameIndex() : 0;
}

size_t BufferFillingWorkerFacade::getFrameSize() const {
    return worker_base_uptr_ ? worker_base_uptr_->getFrameSize() : 0;
}

long BufferFillingWorkerFacade::getFileSize() const {
    return worker_base_uptr_ ? worker_base_uptr_->getFileSize() : 0;
}

int BufferFillingWorkerFacade::getWidth() const {
    return worker_base_uptr_ ? worker_base_uptr_->getWidth() : 0;
}

int BufferFillingWorkerFacade::getHeight() const {
    return worker_base_uptr_ ? worker_base_uptr_->getHeight() : 0;
}

int BufferFillingWorkerFacade::getBytesPerPixel() const {
    return worker_base_uptr_ ? worker_base_uptr_->getBytesPerPixel() : 0;
}

const char* BufferFillingWorkerFacade::getPath() const {
    return worker_base_uptr_ ? worker_base_uptr_->getPath() : "";
}

bool BufferFillingWorkerFacade::hasMoreFrames() const {
    return worker_base_uptr_ && worker_base_uptr_->hasMoreFrames();
}

bool BufferFillingWorkerFacade::isAtEnd() const {
    return worker_base_uptr_ && worker_base_uptr_->isAtEnd();
}

// ============ 提供原材料（BufferPool ID）============

uint64_t BufferFillingWorkerFacade::getOutputBufferPoolId(BufferPoolType type) {
    if (worker_base_uptr_) {
        return worker_base_uptr_->getOutputBufferPoolId(type);
    }
    return 0;
}

BufferPoolType BufferFillingWorkerFacade::getPrimaryBufferPoolType() {
    if (worker_base_uptr_) {
        return worker_base_uptr_->getPrimaryBufferPoolType();
    }
    return BufferPoolType::DECODE_VIDEO_PRIMARY;  // 默认值
}

