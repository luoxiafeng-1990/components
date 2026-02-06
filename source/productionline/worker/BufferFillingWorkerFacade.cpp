#include "productionline/worker/BufferFillingWorkerFacade.hpp"
#include "common/Logger.hpp"
#include <stdio.h>

// ============ 构造/析构 ============

BufferFillingWorkerFacade::BufferFillingWorkerFacade(const WorkerConfig& config)
    : config_(config)
    , logger_(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker.Facade")))
{
    if (!worker_base_uptr_) {
        LOG4CPLUS_DEBUG(logger_, "Factory create worker according to config");
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
        LOG4CPLUS_ERROR(logger_, " ERROR: Failed to create worker");
        return false;
    }
    
    // ✅ 纯转发：WorkerBase::open() 会从 worker_config_.data_source.path 获取路径
    // 子类的 open(const char* path) 会根据 path 是否为空自行判断模式
    return worker_base_uptr_->open();
}

void BufferFillingWorkerFacade::close() {
    if (worker_base_uptr_) {
        worker_base_uptr_->close();
    }
}
bool BufferFillingWorkerFacade::setSourceBufferPool(std::weak_ptr<BufferPool> pool_weak) {
    if (!worker_base_uptr_) {
        LOG4CPLUS_ERROR(logger_, " ERROR: Worker not initialized");
        return false;
    }
    
    // ✅ 直接调用基类虚函数，自动多态分发
    // 支持 Buffer 模式的 Worker（如 FFmpegDecodeWorker）
    // 会重写此方法并返回 true，不支持的 Worker 会使用基类默认实现返回 false
    return worker_base_uptr_->setSourceBufferPool(pool_weak);
}

bool BufferFillingWorkerFacade::isOpen() const {
    return worker_base_uptr_ && worker_base_uptr_->isOpen();
}

// ============ 核心功能：填充Buffer ============

/**
 * v2.33 变更：返回类型从 bool 改为 FillResult
 */
FillResult BufferFillingWorkerFacade::fillBuffer(int frame_index, Buffer* buffer) {
    if (!worker_base_uptr_) {
        LOG4CPLUS_ERROR(logger_, " ERROR: Worker not initialized");
        return FillResult::notOpen();
    }
    return worker_base_uptr_->fillBuffer(frame_index, buffer);
}

// ============ 导航操作（门面转发） ============

bool BufferFillingWorkerFacade::seek(int frame_index) {
    if (!worker_base_uptr_) {
        LOG4CPLUS_ERROR(logger_, " ERROR: Worker not initialized");
        return false;
    }
    return worker_base_uptr_->seek(frame_index);
}

bool BufferFillingWorkerFacade::seekToBegin() {
    if (!worker_base_uptr_) {
        LOG4CPLUS_ERROR(logger_, " ERROR: Worker not initialized");
        return false;
    }
    return worker_base_uptr_->seekToBegin();
}

bool BufferFillingWorkerFacade::seekToEnd() {
    if (!worker_base_uptr_) {
        LOG4CPLUS_ERROR(logger_, " ERROR: Worker not initialized");
        return false;
    }
    return worker_base_uptr_->seekToEnd();
}

bool BufferFillingWorkerFacade::skip(int frame_count) {
    if (!worker_base_uptr_) {
        LOG4CPLUS_ERROR(logger_, " ERROR: Worker not initialized");
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

double BufferFillingWorkerFacade::getOutputBytesPerPixel(int channel) const {
    return worker_base_uptr_ ? worker_base_uptr_->getOutputBytesPerPixel(channel) : 0;
}
