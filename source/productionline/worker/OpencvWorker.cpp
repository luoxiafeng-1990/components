#include "productionline/worker/OpencvWorker.hpp"
#include "vendor/taco/decode/TacoDecoderExtension.hpp"
#include "common/Logger.hpp"
#include "bufferpool/pool/base/BufferPool.hpp"
#include "bufferpool/pool/builder/BufferPoolBuilderFactory.hpp"
#include "productionline/worker/base/ComponentTopology.hpp"
#include <string.h>
#include <chrono>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

// FFmpeg headers
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include "ta_sys_api.h"
}

template<size_t N>
size_t collectJpegFiles(const fs::path& path, std::string (&file_list)[N]) {
    size_t count = 0;
    
    if (!fs::exists(path)) {
        return 0;
    }
    
    // 处理单个文件
    if (fs::is_regular_file(path)) {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        
        if (ext == ".jpg" || ext == ".jpeg") {
            file_list[count++] = path.string();
        }
        return count;
    }
    
    // 处理目录
    if (fs::is_directory(path)) {
        for (const auto& entry : fs::directory_iterator(path)) {
            if (count >= N) break;  // 数组已满，停止添加
            
            if (fs::is_regular_file(entry)) {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                
                if (ext == ".jpg" || ext == ".jpeg") {
                    file_list[count++] = entry.path().string();
                }
            }
        }
    }
    
    return count;
}

std::string matInfo(const cv::Mat& mat) {
    int matType = mat.type();
    int depth = matType & CV_MAT_DEPTH_MASK;
    int channels = (matType >> CV_CN_SHIFT) + 1;

    std::string depthStr;
    switch(depth) {
        case CV_8U:  depthStr = "CV_8U"; break;
        case CV_8S:  depthStr = "CV_8S"; break;
        case CV_16U: depthStr = "CV_16U"; break;
        case CV_16S: depthStr = "CV_16S"; break;
        case CV_32S: depthStr = "CV_32S"; break;
        case CV_32F: depthStr = "CV_32F"; break;
        case CV_64F: depthStr = "CV_64F"; break;
        case CV_16F: depthStr = "CV_16F"; break;
        default:     depthStr = "CV_UNKNOWN";
    }

    std::stringstream ss;
    ss << "Mat Info: ";

    // 添加尺寸信息
    if (mat.dims == 2) {
        // 二维矩阵：显示rows和cols
        ss << "Size(" << mat.cols << "x" << mat.rows << ") ";
    } else if (mat.dims > 2) {
        // 多维矩阵：显示各个维度
        ss << "Dims[" << mat.dims << "] Size[";
        for (int i = 0; i < mat.dims; ++i) {
            ss << mat.size[i];
            if (i < mat.dims - 1) ss << "x";
        }
        ss << "] ";
    } else {
        // 空矩阵或无维度
        ss << "Empty ";
    }

    // 添加类型信息
    ss << depthStr << "C" << channels;

    // 添加更多详细信息
    ss << " | Depth:" << depth
       << " | Channels:" << channels
       << " | Total:" << mat.total()
       << " | Continuous:" << (mat.isContinuous() ? "Yes" : "No")
       << " | DmabufHeap:" << (mat.isdmabufheap() ? "Yes" : "No");

    return ss.str();
}

OpencvWorker::OpencvWorker(const WorkerConfig& config)
    : WorkerBase(BufferPoolBuilderFactory::AllocatorType::MAT, config)  // 传递 config 给父类
    , logger(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("components.Worker.OpenCV")))
    , file_path(config.data_source.path)
    , file_num(0)
    , current_file_index (-1)
    , use_hardware (config.decoder.enable_hardware)
    , use_mock (config.decoder.use_mock)
    , src_height (config.decoder.mock_src_height)
    , src_width (config.decoder.mock_src_width)
    , pix_fmt (static_cast<AVPixelFormat>(config.decoder.pix_fmt))
    {
    LOG4CPLUS_DEBUG(logger, " OpencvWorker created with config");
    
    if (file_path.empty()) LOG4CPLUS_ERROR(logger, "data_source.path is empty");
    file_num = collectJpegFiles(file_path, file_list_);
    if (file_num == 0) LOG4CPLUS_ERROR(logger, "Jpeg files are not included");

    for (size_t i = 0 ; i < file_num; i++){
        std::cout << file_list_[i] << std::endl;
    }

    if (use_mock) {
        LOG4CPLUS_INFO(logger, "mock data");
        file_num = 100000;
    }
    if (config.data_source.max_frames > 0){
        file_num = file_num < static_cast<size_t>(config.data_source.max_frames) ? file_num : static_cast<size_t>(config.data_source.max_frames);
    }
    LOG4CPLUS_DEBUG_FMT(logger, "for '%s' jpg num=%zu", file_path.c_str(), file_num);
}

OpencvWorker::~OpencvWorker() {
    LOG4CPLUS_DEBUG(logger, "🧹 Destroying OpencvWorker...");
    
    if (!buffer_pool_type_map_.empty()) {
        LOG4CPLUS_DEBUG(logger, "手动清理 BufferPool 和 AVFrame...");
        builder_->destroyPool();  // 释放所有 Pool 中的 Buffer 和 AVFrame
        clearAllBufferPools();
    }
    
    LOG4CPLUS_DEBUG(logger, "🧹 OpencvWorker destroyed");
}

// ============ IVideoReader 接口实现 ============

bool OpencvWorker::open(const char* path) {
    open();
    return true;
}

bool OpencvWorker::open() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // 6. 生成 BufferPool 名称
    std::string pool_name;
    pool_name = std::string("OpenCVWorker_") + file_path;
    
    uint64_t pool_id = builder_->allocatePoolWithBuffers(
        worker_config_.data_source.buffer_count,
        0,
        pool_name,
        "NORMAL_MODE"
    );
    
    if (pool_id == 0) {
        LOG4CPLUS_ERROR(logger, " Failed to create BufferPool via Allocator");
        return false;
    }
    
    // 7. ✅ v2.18 修复：统一注册 BufferPool（Buffer 和 RTSP 模式都需要）
    if (!registerBufferPool(BufferPoolType::DECODE_VIDEO_PRIMARY, pool_id)) {
        LOG4CPLUS_ERROR(logger, " Failed to register BufferPool");
        return false;
    }
    
    // 8. 从 ComponentTopology 获取 Pool 名称
    auto pool_weak = ComponentTopology::getInstance().getPool(pool_id);
    auto pool = pool_weak.lock();
    std::string actual_pool_name = pool ? pool->getName() : "Unknown";
    
    LOG4CPLUS_DEBUG_FMT(logger, "    Source: %s", file_path.c_str());
    LOG4CPLUS_DEBUG_FMT(logger, "    BufferPool: '%s' (ID: %lu, %d buffers)", 
                  actual_pool_name.c_str(), pool_id, 
                  worker_config_.data_source.buffer_count);
    
    return true;
}

// ============ v2.13 EncodedPacketSourceFromBuffer 配置 ============

bool OpencvWorker::setSourceBufferPool(std::weak_ptr<BufferPool> pool_weak) {    
    return true;
}

void OpencvWorker::close() {    
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        
        LOG4CPLUS_INFO(logger, "🛑 Closing video source...");
        
        // ⭐ 清除所有 BufferPool 注册（标记不再使用）
        clearAllBufferPools();
    }
    
    LOG4CPLUS_DEBUG(logger, " Video source closed");
}

bool OpencvWorker::isOpen() const {
    return true;
}

bool OpencvWorker::seek(int frame_index) {
    return false;
}

bool OpencvWorker::seekToBegin() {
    return false;
}

bool OpencvWorker::seekToEnd() {
    return false;
}

bool OpencvWorker::skip(int frame_count) {
    return false;
}


int OpencvWorker::getTotalFrames() const {
    if (use_mock) return 100000;
    else return static_cast<int>(file_num);
}

int OpencvWorker::getCurrentFrameIndex() const {
    return 0;
}

size_t OpencvWorker::getFrameSize() const {
    return 0;
}

long OpencvWorker::getFileSize() const {
    return -1;
}

int OpencvWorker::getSourceWidth() const {
    return 0;
}

int OpencvWorker::getSourceHeight() const {
    return 0;
}

int OpencvWorker::getOutputWidth() const {
    return 0;
}

int OpencvWorker::getOutputHeight() const {
    return 0;
}

double OpencvWorker::getOutputBytesPerPixel(int channel) const {
    return 0.0;  // 其他通道不支持
}

std::string OpencvWorker::getPath() const {
    return file_path;
}

IDataSourceNavigator::SourceType OpencvWorker::getDataSourceType() const {
    return SourceType::FILE_SOURCE;  // 默认是网络流类型
}

bool OpencvWorker::hasMoreFrames() const {
    return true;
}

bool OpencvWorker::isAtEnd() const {
    if (current_file_index >= file_num) return true;
    else return false;
}

FillResult OpencvWorker::fillBuffer(int frame_index, Buffer* buffer) {
    if (frame_index >= static_cast<int>(file_num)) return FillResult::fromCodec(CodecSendResult::eof());
    if (!buffer) {
        LOG4CPLUS_ERROR(logger, " ERROR: buffer is nullptr");
        return FillResult::invalidParam();
    }
    
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    cv::Mat* old_mat_ptr = buffer->getMat();
    delete old_mat_ptr;
    cv::Mat* mat_ptr;
    if (use_mock){
        mat_ptr = new cv::Mat(mockMat(src_width,src_height,use_hardware,pix_fmt));
        std::cout << "[mock hw=" << (use_hardware ? "true" : "false") << "]" << matInfo(*mat_ptr) << std::endl;
    }
    else{
        int flags;
        if (use_hardware && pix_fmt == AV_PIX_FMT_NV12) flags = cv::IMREAD_COLOR_YUV; //硬件，输出nv12
        else if (use_hardware && pix_fmt == AV_PIX_FMT_BGR24) flags = cv::IMREAD_COLOR; //硬件，输出bgr888
        else flags = cv::IMREAD_COLOR|cv::IMREAD_RETRY_SOFTDEC; //软件，输出bgr888
        mat_ptr = new cv::Mat(cv::imread(file_list_[frame_index],flags));
    }
    
    buffer->setMat(mat_ptr);

    if (mat_ptr->empty()) {
        LOG4CPLUS_WARN_FMT(logger, "Worker [Mat %d] imread failed: %s",
            frame_index, file_list_[frame_index].c_str());
    }

    current_file_index++;
    
    return FillResult::success();
}

cv::Mat OpencvWorker::mockMat(int width, int height, bool hw, AVPixelFormat pix_fmt){
    cv::Mat dst;

    if (hw) {
        // ===== 硬件路径：分配在 dmabufheap =====
        if (pix_fmt == AV_PIX_FMT_NV12 || pix_fmt == AV_PIX_FMT_NV21) {
            // NV12/NV21：使用 cv::av::create 创建 AVFrame（分配 dmabufheap）
            // 然后通过 Mat::create(AVFrame*) 继承 dmabufheap 属性
            AVFrame* frame = cv::av::create(height, width);
            dst.create(frame);
        } else if (pix_fmt == AV_PIX_FMT_BGR24 || pix_fmt == AV_PIX_FMT_RGB24) {
            // BGR/RGB：Mat::create(rows, cols, CV_8UC3) 当 size>=48 时
            // 会自动使用 hal::getAllocator()（dmabufheap），见 matrix.cpp:840-841
            dst.create(height, width, CV_8UC3);
        }
    } else {
        // ===== 软件路径：普通内存分配 =====
        if (pix_fmt == AV_PIX_FMT_NV12 || pix_fmt == AV_PIX_FMT_NV21) {
            // NV12 打包格式：Y平面(h) + UV平面(h/2)，总高度 = h * 3/2
            dst.create(height * 3 / 2, width, CV_8UC1);
        } else if (pix_fmt == AV_PIX_FMT_BGR24 || pix_fmt == AV_PIX_FMT_RGB24) {
            dst.create(height, width, CV_8UC3);
        }
    }

    // 填充随机数据
    if (pix_fmt == AV_PIX_FMT_NV12 || pix_fmt == AV_PIX_FMT_NV21) {
        cv::randu(dst, cv::Scalar(0), cv::Scalar(255));
    } else if (pix_fmt == AV_PIX_FMT_BGR24 || pix_fmt == AV_PIX_FMT_RGB24) {
        cv::randu(dst, cv::Scalar(0, 0, 0), cv::Scalar(255, 255, 255));
    }

    return dst;
}

const AVCodecParameters* OpencvWorker::getCodecParameters() const {
    return nullptr;
}

AVPixelFormat OpencvWorker::getSourcePixelFormat() const {
    return AV_PIX_FMT_NONE;
}

BufferPoolType OpencvWorker::getPrimaryBufferPoolType() const {
    return BufferPoolType::DECODE_VIDEO_PRIMARY;
}