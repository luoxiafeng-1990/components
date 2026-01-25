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
        LOG4CPLUS_ERROR(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker.Facade")), "[Worker] ERROR: Failed to create worker");
        return false;
    }
    
    // ⭐ v2.9新增：检查是否是 Buffer 模式
    // ⭐ v2.22 重构：数据源配置从 decoder 移至 datasource
    bool is_buffer_mode = config_.data_source.buffer_mode;
    
    if (is_buffer_mode) {
        // Buffer 模式：不需要文件路径，直接调用 open(nullptr)
        LOG4CPLUS_DEBUG(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker.Facade")), "[Worker] BufferFillingWorkerFacade: Opening in Buffer mode (no file path needed)");
        return worker_base_uptr_->open(nullptr);
    }
    
    // 文件模式：从 config_ 获取所有参数
    const std::string& file_path = config_.data_source.path;
    
    if (file_path.empty()) {
        LOG4CPLUS_ERROR(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker.Facade")), "[Worker] ERROR: File path not set in config");
        return false;
    }
    
    const char* path = file_path.c_str();
    
    LOG4CPLUS_DEBUG_FMT(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker.Facade")), "[Worker] BufferFillingWorkerFacade: Opening file: %s", path);
    return worker_base_uptr_->open(path);
}

void BufferFillingWorkerFacade::close() {
    if (worker_base_uptr_) {
        worker_base_uptr_->close();
    }
}
bool BufferFillingWorkerFacade::setSourceBufferPool(std::weak_ptr<BufferPool> pool_weak) {
    if (!worker_base_uptr_) {
        LOG4CPLUS_ERROR(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker.Facade")), "[Worker] ERROR: Worker not initialized");
        return false;
    }
    
    // ✅ 直接调用基类虚函数，自动多态分发
    // 支持 Buffer 模式的 Worker（如 FfmpegDecodeVideoFileWorker、FfmpegDecodeRtspWorker）
    // 会重写此方法并返回 true，不支持的 Worker 会使用基类默认实现返回 false
    return worker_base_uptr_->setSourceBufferPool(pool_weak);
}

bool BufferFillingWorkerFacade::isOpen() const {
    return worker_base_uptr_ && worker_base_uptr_->isOpen();
}

// ============ 核心功能：填充Buffer ============

bool BufferFillingWorkerFacade::fillBuffer(int frame_index, Buffer* buffer) {
    if (!worker_base_uptr_) {
        LOG4CPLUS_ERROR(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker.Facade")), "[Worker] ERROR: Worker not initialized");
        return false;
    }
    return worker_base_uptr_->fillBuffer(frame_index, buffer);
}

// ============ 导航操作（门面转发） ============

bool BufferFillingWorkerFacade::seek(int frame_index) {
    if (!worker_base_uptr_) {
        LOG4CPLUS_ERROR(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker.Facade")), "[Worker] ERROR: Worker not initialized");
        return false;
    }
    return worker_base_uptr_->seek(frame_index);
}

bool BufferFillingWorkerFacade::seekToBegin() {
    if (!worker_base_uptr_) {
        LOG4CPLUS_ERROR(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker.Facade")), "[Worker] ERROR: Worker not initialized");
        return false;
    }
    return worker_base_uptr_->seekToBegin();
}

bool BufferFillingWorkerFacade::seekToEnd() {
    if (!worker_base_uptr_) {
        LOG4CPLUS_ERROR(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker.Facade")), "[Worker] ERROR: Worker not initialized");
        return false;
    }
    return worker_base_uptr_->seekToEnd();
}

bool BufferFillingWorkerFacade::skip(int frame_count) {
    if (!worker_base_uptr_) {
        LOG4CPLUS_ERROR(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker.Facade")), "[Worker] ERROR: Worker not initialized");
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

std::string BufferFillingWorkerFacade::getPath() const {
    if (worker_base_uptr_) {
        return worker_base_uptr_->getPath();
    }
    return std::string();
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

// ============ 数据源属性（v2.14）============

int BufferFillingWorkerFacade::getSourceWidth() const {
    return worker_base_uptr_ ? worker_base_uptr_->getSourceWidth() : 0;
}

int BufferFillingWorkerFacade::getSourceHeight() const {
    return worker_base_uptr_ ? worker_base_uptr_->getSourceHeight() : 0;
}

AVPixelFormat BufferFillingWorkerFacade::getSourcePixelFormat() const {
    return worker_base_uptr_ ? worker_base_uptr_->getSourcePixelFormat() : AV_PIX_FMT_NONE;
}

// ============ 编解码器参数和时间基获取（v2.14）============

const struct AVCodecParameters* BufferFillingWorkerFacade::getSourceCodecParameters() const {
    if (worker_base_uptr_) {
        return worker_base_uptr_->getSourceCodecParameters();
    }
    return nullptr;
}

struct AVRational BufferFillingWorkerFacade::getTimeBase() const {
    if (worker_base_uptr_) {
        return worker_base_uptr_->getTimeBase();
    }
    return {1, 25};  // 默认 25fps
}

// ============ Worker 输出属性（v2.14）============

int BufferFillingWorkerFacade::getOutputWidth() const {
    return worker_base_uptr_ ? worker_base_uptr_->getOutputWidth() : 0;
}

int BufferFillingWorkerFacade::getOutputHeight() const {
    return worker_base_uptr_ ? worker_base_uptr_->getOutputHeight() : 0;
}

double BufferFillingWorkerFacade::getOutputBytesPerPixel() const {
    return worker_base_uptr_ ? worker_base_uptr_->getOutputBytesPerPixel() : 0;
}
