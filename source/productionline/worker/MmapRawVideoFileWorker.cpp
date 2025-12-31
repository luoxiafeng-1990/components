#include "productionline/worker/MmapRawVideoFileWorker.hpp"
#include "common/Logger.hpp"
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>

// ============ 构造函数 ============

MmapRawVideoFileWorker::MmapRawVideoFileWorker()
    : WorkerBase(BufferAllocatorFactory::AllocatorType::NORMAL)  // 🎯 只需传递类型！
    , fd_(-1)
    , mapped_file_ptr_(nullptr)
    , mapped_size_(0)
    , width_(0)
    , height_(0)
    , bits_per_pixel_(0)
    , frame_size_(0)
    , file_size_(0)
    , total_frames_(0)
    , current_frame_index_(0)
    , is_open_(false)
    , detected_format_(FileFormat::UNKNOWN)
{
    // path_ 使用 std::string，无需手动初始化
    // 🎯 父类已经创建好 NORMAL 类型的 allocator_facade_，无需任何初始化代码
}

// v2.2: 配置构造函数（新增）
MmapRawVideoFileWorker::MmapRawVideoFileWorker(const WorkerConfig& config)
    : WorkerBase(BufferAllocatorFactory::AllocatorType::NORMAL, config)  // 传递 config 给父类
    , fd_(-1)
    , mapped_file_ptr_(nullptr)
    , mapped_size_(0)
    , width_(0)
    , height_(0)
    , bits_per_pixel_(0)
    , frame_size_(0)
    , file_size_(0)
    , total_frames_(0)
    , current_frame_index_(0)
    , is_open_(false)
    , detected_format_(FileFormat::UNKNOWN)
{
    // path_ 使用 std::string，无需手动初始化
}

MmapRawVideoFileWorker::~MmapRawVideoFileWorker() {
    close();
}

// ============ IVideoReader 接口实现 ============

bool MmapRawVideoFileWorker::open(const char* path) {
    if (is_open_) {
        LOG_WARN_FMT("[Worker]  Warning: File already opened, closing previous file");
        close();
    }
    
    path_ = path;  // 使用 std::string 自动管理
    
    LOG_INFO_FMT("📂 Opening video file: %s\n", path);
    LOG_INFO_FMT("   Mode: Auto-detect format");
    LOG_INFO_FMT("   Worker: MmapRawVideoFileWorker (memory-mapped I/O)");
    
    fd_ = ::open(path, O_RDONLY);
    if (fd_ < 0) {
        LOG_ERROR_FMT("[Worker] ERROR: Cannot open file: %s", strerror(errno));
        return false;
    }
    
    detected_format_ = detectFileFormat();
    
    switch (detected_format_) {
        case FileFormat::MP4:
            LOG_INFO_FMT("📹 Detected format: MP4");
            if (!parseMP4Header()) {
                ::close(fd_);
                fd_ = -1;
                return false;
            }
            break;
            
        case FileFormat::H264:
            LOG_INFO_FMT("📹 Detected format: H.264");
            if (!parseH264Header()) {
                ::close(fd_);
                fd_ = -1;
                return false;
            }
            break;
            
        case FileFormat::H265:
            LOG_INFO_FMT("📹 Detected format: H.265");
            LOG_ERROR_FMT("[Worker] ERROR: H.265 format not yet supported");
            ::close(fd_);
            fd_ = -1;
            return false;
            
        case FileFormat::AVI:
            LOG_INFO_FMT("📹 Detected format: AVI");
            LOG_ERROR_FMT("[Worker] ERROR: AVI format not yet supported");
            ::close(fd_);
            fd_ = -1;
            return false;
            
        case FileFormat::RAW:
        case FileFormat::UNKNOWN:
            LOG_ERROR_FMT("[Worker] ERROR: No format magic detected");
            LOG_INFO_FMT("   This file may be raw format or unsupported encoded format");
            LOG_INFO_FMT("   ");
            LOG_INFO_FMT("   💡 For raw format, please use:");
            LOG_INFO_FMT("      open(path, width, height, bits_per_pixel)");
            ::close(fd_);
            fd_ = -1;
            return false;
    }
    
    if (!validateFile()) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    
    if (!mapFile()) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    
    is_open_ = true;
    current_frame_index_ = 0;
    
    LOG_DEBUG_FMT("[Worker] Video file opened successfully");
    LOG_INFO_FMT("   Format: ");
    switch (detected_format_) {
        case FileFormat::RAW:  LOG_INFO_FMT("RAW"); break;
        case FileFormat::MP4:  LOG_INFO_FMT("MP4"); break;
        case FileFormat::H264: LOG_INFO_FMT("H.264"); break;
        case FileFormat::H265: LOG_INFO_FMT("H.265"); break;
        case FileFormat::AVI:  LOG_INFO_FMT("AVI"); break;
        default: LOG_INFO_FMT("UNKNOWN"); break;
    }
    LOG_INFO_FMT("   Resolution: %dx%d\n", width_, height_);
    LOG_INFO_FMT("   Bits per pixel: %d\n", bits_per_pixel_);
    LOG_INFO_FMT("   Frame size: %zu bytes\n", frame_size_);
    LOG_INFO_FMT("   File size: %ld bytes\n", file_size_);
    LOG_INFO_FMT("   Total frames: %d\n", total_frames_);
    
    return true;
}

bool MmapRawVideoFileWorker::open(const char* path, int width, int height, int bits_per_pixel) {
    if (is_open_) {
        LOG_WARN_FMT("[Worker]  Warning: File already opened, closing previous file");
        close();
    }
    
    if (width <= 0 || height <= 0 || bits_per_pixel <= 0) {
        LOG_ERROR_FMT("[Worker] ERROR: Invalid parameters");
        LOG_INFO_FMT("   width=%d, height=%d, bits_per_pixel=%d\n", 
               width, height, bits_per_pixel);
        return false;
    }
    
    path_ = path;  // 使用 std::string 自动管理
    width_ = width;
    height_ = height;
    bits_per_pixel_ = bits_per_pixel;
    
    size_t total_bits = (size_t)width_ * height_ * bits_per_pixel_;
    frame_size_ = (total_bits + 7) / 8;
    
    detected_format_ = FileFormat::RAW;
    
    LOG_INFO_FMT("📂 Opening raw video file: %s\n", path);
    LOG_INFO_FMT("   Format: %dx%d, %d bits per pixel\n", 
           width_, height_, bits_per_pixel_);
    LOG_INFO_FMT("   Frame size: %zu bytes\n", frame_size_);
    LOG_INFO_FMT("   Worker: MmapRawVideoFileWorker (memory-mapped I/O)");
    
    fd_ = ::open(path, O_RDONLY);
    if (fd_ < 0) {
        LOG_ERROR_FMT("[Worker] ERROR: Cannot open file: %s", strerror(errno));
        return false;
    }
    
    if (!validateFile()) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    
    if (!mapFile()) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    
    is_open_ = true;
    current_frame_index_ = 0;
    
    LOG_DEBUG_FMT("[Worker] Raw video file opened successfully");
    LOG_INFO_FMT("   File size: %ld bytes\n", file_size_);
    LOG_INFO_FMT("   Total frames: %d\n", total_frames_);
    
    return true;
}

void MmapRawVideoFileWorker::close() {
    if (!is_open_) {
        return;
    }
    
    unmapFile();
    
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    
    is_open_ = false;
    current_frame_index_ = 0;
    
    LOG_DEBUG_FMT("[Worker] Video file closed: %s", path_.c_str());
}

bool MmapRawVideoFileWorker::isOpen() const {
    return is_open_;
}


bool MmapRawVideoFileWorker::seek(int frame_index) {
    if (!is_open_) {
        LOG_ERROR_FMT("[Worker] ERROR: File not opened");
        return false;
    }
    
    if (frame_index < 0 || frame_index >= total_frames_) {
        LOG_ERROR_FMT("[Worker] ERROR: Invalid frame index %d (valid: 0-%d)\n",
               frame_index, total_frames_ - 1);
        return false;
    }
    
    current_frame_index_ = frame_index;
    return true;
}

bool MmapRawVideoFileWorker::seekToBegin() {
    return seek(0);
}

bool MmapRawVideoFileWorker::seekToEnd() {
    if (!is_open_) {
        LOG_ERROR_FMT("[Worker] ERROR: File not opened");
        return false;
    }
    
    current_frame_index_ = total_frames_;
    return true;
}

bool MmapRawVideoFileWorker::skip(int frame_count) {
    int target_frame = current_frame_index_ + frame_count;
    return seek(target_frame);
}

int MmapRawVideoFileWorker::getTotalFrames() const {
    return total_frames_;
}

int MmapRawVideoFileWorker::getCurrentFrameIndex() const {
    return current_frame_index_;
}

size_t MmapRawVideoFileWorker::getFrameSize() const {
    return frame_size_;
}

long MmapRawVideoFileWorker::getFileSize() const {
    return file_size_;
}

int MmapRawVideoFileWorker::getWidth() const {
    return width_;
}

int MmapRawVideoFileWorker::getHeight() const {
    return height_;
}

int MmapRawVideoFileWorker::getBytesPerPixel() const {
    return (bits_per_pixel_ + 7) / 8;
}

const char* MmapRawVideoFileWorker::getPath() const {
    return path_.c_str();
}

bool MmapRawVideoFileWorker::hasMoreFrames() const {
    return current_frame_index_ < total_frames_;
}

bool MmapRawVideoFileWorker::isAtEnd() const {
    return current_frame_index_ >= total_frames_;
}

// ============================================================================
// 核心功能：填充Buffer
// ============================================================================

bool MmapRawVideoFileWorker::fillBuffer(int frame_index, Buffer* buffer) {
    if (!buffer || !buffer->data()) {
        LOG_ERROR_FMT("[Worker] ERROR: Invalid buffer");
        return false;
    }
    
    if (!is_open_) {
        LOG_ERROR_FMT("[Worker] ERROR: Worker is not open");
        return false;
    }
    
    if (frame_index < 0 || frame_index >= total_frames_) {
        LOG_ERROR_FMT("[Worker] ERROR: Invalid frame index %d (valid: 0-%d)\n",
               frame_index, total_frames_ - 1);
        return false;
    }
    
    if (buffer->size() < frame_size_) {
        LOG_ERROR_FMT("[Worker] ERROR: Buffer too small (need %zu, got %zu)\n", 
               frame_size_, buffer->size());
        return false;
    }
    
    // 从mmap区域拷贝数据到buffer
    size_t frame_offset = (size_t)frame_index * frame_size_;
    const char* frame_addr = (const char*)mapped_file_ptr_ + frame_offset;
    
    memcpy(buffer->data(), frame_addr, frame_size_);
    
    return true;
}

// ============ 内部辅助方法 ============

bool MmapRawVideoFileWorker::validateFile() {
    struct stat st;
    if (fstat(fd_, &st) < 0) {
        LOG_ERROR_FMT("[Worker] ERROR: Cannot get file size: %s", strerror(errno));
        return false;
    }
    
    file_size_ = st.st_size;
    
    if (file_size_ == 0) {
        LOG_ERROR_FMT("[Worker] ERROR: File is empty");
        return false;
    }
    
    total_frames_ = file_size_ / frame_size_;
    
    if (total_frames_ == 0) {
        LOG_ERROR_FMT("[Worker] ERROR: File too small (size=%ld, frame_size=%zu)\n",
               file_size_, frame_size_);
        return false;
    }
    
    if (file_size_ % frame_size_ != 0) {
        LOG_WARN_FMT("[Worker]  Warning: File size (%ld) not aligned to frame size (%zu)\n",
               file_size_, frame_size_);
        LOG_INFO_FMT("   Last frame may be incomplete");
    }
    
    return true;
}

MmapRawVideoFileWorker::FileFormat MmapRawVideoFileWorker::detectFileFormat() {
    unsigned char header[32];
    ssize_t bytes_read = readFileHeader(header, sizeof(header));
    
    if (bytes_read < 16) {
        LOG_WARN_FMT("[Worker]  Warning: Cannot read enough header data");
        return FileFormat::UNKNOWN;
    }
    
    // 检测 MP4 (ftyp box)
    if (bytes_read >= 8 && 
        header[4] == 0x66 && header[5] == 0x74 && 
        header[6] == 0x79 && header[7] == 0x70) {
        return FileFormat::MP4;
    }
    
    // 检测 AVI (RIFF header)
    if (bytes_read >= 12 &&
        header[0] == 0x52 && header[1] == 0x49 && 
        header[2] == 0x46 && header[3] == 0x46 &&
        header[8] == 0x41 && header[9] == 0x56 && 
        header[10] == 0x49 && header[11] == 0x20) {
        return FileFormat::AVI;
    }
    
    // 检测 H.264 (NAL unit start code)
    if (bytes_read >= 4) {
        if ((header[0] == 0x00 && header[1] == 0x00 && 
             header[2] == 0x00 && header[3] == 0x01) ||
            (header[0] == 0x00 && header[1] == 0x00 && header[2] == 0x01)) {
            
            int nal_byte_idx = (header[3] == 0x01) ? 4 : 3;
            if (bytes_read > nal_byte_idx) {
                unsigned char nal_type = header[nal_byte_idx] & 0x1F;
                if (nal_type >= 1 && nal_type <= 21) {
                    return FileFormat::H264;
                }
                if (nal_type <= 40) {
                    return FileFormat::H265;
                }
            }
        }
    }
    
    return FileFormat::UNKNOWN;
}

ssize_t MmapRawVideoFileWorker::readFileHeader(unsigned char* header, size_t size) {
    if (fd_ < 0) {
        return -1;
    }
    
    off_t current_pos = lseek(fd_, 0, SEEK_CUR);
    
    if (lseek(fd_, 0, SEEK_SET) < 0) {
        return -1;
    }
    
    ssize_t bytes_read = read(fd_, header, size);
    
    lseek(fd_, current_pos, SEEK_SET);
    
    return bytes_read;
}

bool MmapRawVideoFileWorker::parseMP4Header() {
    LOG_WARN_FMT("[Worker]  MP4 format detected but not yet fully supported");
    LOG_INFO_FMT("   Please use a tool to extract raw frames, or provide format info");
    return false;
}

bool MmapRawVideoFileWorker::parseH264Header() {
    LOG_WARN_FMT("[Worker]  H.264 format detected but not yet fully supported");
    LOG_INFO_FMT("   Please use a tool to extract raw frames, or provide format info");
    return false;
}

bool MmapRawVideoFileWorker::mapFile() {
    if (fd_ < 0) {
        LOG_ERROR_FMT("[Worker] ERROR: Invalid file descriptor");
        return false;
    }
    
    if (file_size_ <= 0) {
        LOG_ERROR_FMT("[Worker] ERROR: Invalid file size: %ld", file_size_);
        return false;
    }
    
    mapped_file_ptr_ = mmap(NULL, file_size_, 
                        PROT_READ, MAP_PRIVATE, 
                        fd_, 0);
    
    if (mapped_file_ptr_ == MAP_FAILED) {
        LOG_ERROR_FMT("[Worker] ERROR: mmap failed: %s", strerror(errno));
        mapped_file_ptr_ = nullptr;
        return false;
    }
    
    mapped_size_ = file_size_;
    
    LOG_INFO_FMT("🗺️  File mapped to memory: address=%p, size=%zu bytes\n", 
           mapped_file_ptr_, mapped_size_);
    
    return true;
}

void MmapRawVideoFileWorker::unmapFile() {
    if (mapped_file_ptr_ != nullptr && mapped_size_ > 0) {
        if (munmap(mapped_file_ptr_, mapped_size_) < 0) {
            LOG_WARN_FMT("[Worker]  Warning: munmap failed: %s", strerror(errno));
        }
        mapped_file_ptr_ = nullptr;
        mapped_size_ = 0;
    }
}








