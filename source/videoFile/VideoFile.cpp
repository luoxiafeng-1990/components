#include "../../include/videoFile/VideoFile.hpp"
#include <stdio.h>
#include <stdlib.h>  // For atoi
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>  // For mmap/munmap
#include <string.h>
#include <errno.h>

// ============ 构造函数 ============

VideoFile::VideoFile()
    : fd_(-1)
    , mapped_file_(nullptr)
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
    path_[0] = '\0';  // Initialize path as empty string
}

VideoFile::~VideoFile() {
    close();
}

// ============ 文件操作 ============

bool VideoFile::open(const char* path) {
    if (is_open_) {
        printf("⚠️  Warning: File already opened, closing previous file\n");
        close();
    }
    
    // 保存路径
    strncpy(path_, path, MAX_PATH_LENGTH - 1);
    path_[MAX_PATH_LENGTH - 1] = '\0';
    
    printf("📂 Opening video file: %s\n", path);
    printf("   Mode: Auto-detect format\n");
    
    // 打开文件
    fd_ = ::open(path, O_RDONLY);
    if (fd_ < 0) {
        printf("❌ ERROR: Cannot open file: %s\n", strerror(errno));
        return false;
    }
    
    // 检测文件格式
    detected_format_ = detectFileFormat();
    
    switch (detected_format_) {
        case FileFormat::MP4:
            printf("📹 Detected format: MP4\n");
            if (!parseMP4Header()) {
                ::close(fd_);
                fd_ = -1;
                return false;
            }
            break;
            
        case FileFormat::H264:
            printf("📹 Detected format: H.264\n");
            if (!parseH264Header()) {
                ::close(fd_);
                fd_ = -1;
                return false;
            }
            break;
            
        case FileFormat::H265:
            printf("📹 Detected format: H.265\n");
            printf("❌ ERROR: H.265 format not yet supported\n");
            ::close(fd_);
            fd_ = -1;
            return false;
            
        case FileFormat::AVI:
            printf("📹 Detected format: AVI\n");
            printf("❌ ERROR: AVI format not yet supported\n");
            ::close(fd_);
            fd_ = -1;
            return false;
            
        case FileFormat::RAW:
        case FileFormat::UNKNOWN:
            printf("❌ ERROR: No format magic detected\n");
            printf("   This file may be raw format or unsupported encoded format\n");
            printf("   \n");
            printf("   💡 For raw format, please use:\n");
            printf("      openRaw(path, width, height, bits_per_pixel)\n");
            ::close(fd_);
            fd_ = -1;
            return false;
    }
    
    // 验证文件
    if (!validateFile()) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    
    // mmap映射文件
    if (!mapFile()) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    
    is_open_ = true;
    current_frame_index_ = 0;
    
    printf("✅ Video file opened successfully\n");
    printf("   Format: ");
    switch (detected_format_) {
        case FileFormat::RAW:  printf("RAW\n"); break;
        case FileFormat::MP4:  printf("MP4\n"); break;
        case FileFormat::H264: printf("H.264\n"); break;
        case FileFormat::H265: printf("H.265\n"); break;
        case FileFormat::AVI:  printf("AVI\n"); break;
        default: printf("UNKNOWN\n"); break;
    }
    printf("   Resolution: %dx%d\n", width_, height_);
    printf("   Bits per pixel: %d\n", bits_per_pixel_);
    printf("   Frame size: %zu bytes\n", frame_size_);
    printf("   File size: %ld bytes\n", file_size_);
    printf("   Total frames: %d\n", total_frames_);
    
    return true;
}

bool VideoFile::openRaw(const char* path, int width, int height, int bits_per_pixel) {
    if (is_open_) {
        printf("⚠️  Warning: File already opened, closing previous file\n");
        close();
    }
    
    // 验证参数
    if (width <= 0 || height <= 0 || bits_per_pixel <= 0) {
        printf("❌ ERROR: Invalid parameters\n");
        printf("   width=%d, height=%d, bits_per_pixel=%d\n", 
               width, height, bits_per_pixel);
        return false;
    }
    
    // 保存参数
    strncpy(path_, path, MAX_PATH_LENGTH - 1);
    path_[MAX_PATH_LENGTH - 1] = '\0';
    width_ = width;
    height_ = height;
    bits_per_pixel_ = bits_per_pixel;
    
    // 计算帧大小：总位数 / 8 向上取整
    // 对于非整数字节的像素格式（如12bit），这样可以确保分配足够的内存
    size_t total_bits = (size_t)width_ * height_ * bits_per_pixel_;
    frame_size_ = (total_bits + 7) / 8;  // 向上取整到字节
    
    detected_format_ = FileFormat::RAW;
    
    printf("📂 Opening raw video file: %s\n", path);
    printf("   Format: %dx%d, %d bits per pixel\n", 
           width_, height_, bits_per_pixel_);
    printf("   Frame size: %zu bytes\n", frame_size_);
    
    // 打开文件
    fd_ = ::open(path, O_RDONLY);
    if (fd_ < 0) {
        printf("❌ ERROR: Cannot open file: %s\n", strerror(errno));
        return false;
    }
    
    // 验证文件
    if (!validateFile()) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    
    // mmap映射文件
    if (!mapFile()) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    
    is_open_ = true;
    current_frame_index_ = 0;
    
    printf("✅ Raw video file opened successfully\n");
    printf("   File size: %ld bytes\n", file_size_);
    printf("   Total frames: %d\n", total_frames_);
    
    return true;
}

void VideoFile::close() {
    if (!is_open_) {
        return;
    }
    
    // 解除内存映射
    unmapFile();
    
    // 关闭文件描述符
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    
    is_open_ = false;
    current_frame_index_ = 0;
    
    printf("✅ Video file closed: %s\n", path_);
}

bool VideoFile::isOpen() const {
    return is_open_;
}

// ============ 读取操作 ============

bool VideoFile::readFrameTo(Buffer& dest_buffer) {
    return readFrameTo(dest_buffer.data(), dest_buffer.size());
}

bool VideoFile::readFrameTo(void* dest_buffer, size_t buffer_size) {
    if (!is_open_) {
        printf("❌ ERROR: File not opened\n");
        return false;
    }
    
    if (!dest_buffer) {
        printf("❌ ERROR: Destination buffer is null\n");
        return false;
    }
    
    if (buffer_size < frame_size_) {
        printf("❌ ERROR: Buffer too small (need %zu, got %zu)\n", 
               frame_size_, buffer_size);
        return false;
    }
    
    // 检查是否超出文件范围
    if (current_frame_index_ >= total_frames_) {
        printf("⚠️  Warning: Reached end of file\n");
        return false;
    }
    
    // 计算当前帧在映射内存中的地址
    size_t frame_offset = (size_t)current_frame_index_ * frame_size_;
    const char* frame_addr = (const char*)mapped_file_ + frame_offset;
    
    // 从映射内存拷贝数据（代替read系统调用）
    memcpy(dest_buffer, frame_addr, frame_size_);
    
    current_frame_index_++;
    return true;
}

bool VideoFile::readFrameAt(int frame_index, Buffer& dest_buffer) {
    return readFrameAt(frame_index, dest_buffer.data(), dest_buffer.size());
}

bool VideoFile::readFrameAt(int frame_index, void* dest_buffer, size_t buffer_size) {
    if (!seek(frame_index)) {
        return false;
    }
    
    return readFrameTo(dest_buffer, buffer_size);
}

bool VideoFile::readFrameAtThreadSafe(int frame_index, void* dest_buffer, size_t buffer_size) const {
    // 参数检查
    if (!is_open_ || mapped_file_ == nullptr) {
        return false;
    }
    
    if (frame_index < 0 || frame_index >= total_frames_) {
        return false;
    }
    
    if (buffer_size < frame_size_) {
        return false;
    }
    
    // 🔑 关键：直接计算偏移量，不修改任何成员变量
    // 这是线程安全的，因为所有线程都是从只读的mmap内存中读取
    size_t frame_offset = (size_t)frame_index * frame_size_;
    const char* frame_addr = (const char*)mapped_file_ + frame_offset;
    // 从映射内存拷贝数据（线程安全：不同的dest_buffer，不同的偏移量）
    memcpy(dest_buffer, frame_addr, frame_size_);
    return true;
}

// ============ 导航操作 ============

bool VideoFile::seek(int frame_index) {
    if (!is_open_) {
        printf("❌ ERROR: File not opened\n");
        return false;
    }
    
    if (frame_index < 0 || frame_index >= total_frames_) {
        printf("❌ ERROR: Invalid frame index %d (valid: 0-%d)\n",
               frame_index, total_frames_ - 1);
        return false;
    }
    
    // 使用mmap后，seek只需要更新逻辑位置，无需lseek系统调用
    current_frame_index_ = frame_index;
    return true;
}

bool VideoFile::seekToBegin() {
    return seek(0);
}

bool VideoFile::seekToEnd() {
    if (!is_open_) {
        printf("❌ ERROR: File not opened\n");
        return false;
    }
    
    // 使用mmap后，只需要更新逻辑位置
    current_frame_index_ = total_frames_;
    return true;
}

bool VideoFile::skip(int frame_count) {
    int target_frame = current_frame_index_ + frame_count;
    return seek(target_frame);
}

// ============ 信息查询 ============

int VideoFile::getTotalFrames() const {
    return total_frames_;
}

int VideoFile::getCurrentFrameIndex() const {
    return current_frame_index_;
}

size_t VideoFile::getFrameSize() const {
    return frame_size_;
}

long VideoFile::getFileSize() const {
    return file_size_;
}

// ============ 元数据 ============

int VideoFile::getWidth() const {
    return width_;
}

int VideoFile::getHeight() const {
    return height_;
}

int VideoFile::getBytesPerPixel() const {
    // 注意：这里返回的是向上取整的字节数
    // 例如：12bit -> 2字节，16bit -> 2字节，24bit -> 3字节
    // 实际使用时可能需要根据具体的像素格式进行处理
    return (bits_per_pixel_ + 7) / 8;
}

const char* VideoFile::getPath() const {
    return path_;
}

// ============ 状态查询 ============

bool VideoFile::hasMoreFrames() const {
    return current_frame_index_ < total_frames_;
}

bool VideoFile::isAtEnd() const {
    return current_frame_index_ >= total_frames_;
}

// ============ 内部辅助方法 ============

bool VideoFile::validateFile() {
    // 获取文件大小
    struct stat st;
    if (fstat(fd_, &st) < 0) {
        printf("❌ ERROR: Cannot get file size: %s\n", strerror(errno));
        return false;
    }
    
    file_size_ = st.st_size;
    
    // 检查文件大小
    if (file_size_ == 0) {
        printf("❌ ERROR: File is empty\n");
        return false;
    }
    
    // 计算总帧数
    total_frames_ = file_size_ / frame_size_;
    
    if (total_frames_ == 0) {
        printf("❌ ERROR: File too small (size=%ld, frame_size=%zu)\n",
               file_size_, frame_size_);
        return false;
    }
    
    // 检查是否有不完整的帧
    if (file_size_ % frame_size_ != 0) {
        printf("⚠️  Warning: File size (%ld) not aligned to frame size (%zu)\n",
               file_size_, frame_size_);
        printf("   Last frame may be incomplete\n");
    }
    
    return true;
}

// ============ 格式检测辅助方法 ============

VideoFile::FileFormat VideoFile::detectFileFormat() {
    unsigned char header[32];
    ssize_t bytes_read = readFileHeader(header, sizeof(header));
    
    if (bytes_read < 16) {
        printf("⚠️  Warning: Cannot read enough header data\n");
        return FileFormat::UNKNOWN;
    }
    
    // 检测 MP4 (ftyp box)
    // MP4 格式: 00 00 00 xx 66 74 79 70
    if (bytes_read >= 8 && 
        header[4] == 0x66 && header[5] == 0x74 && 
        header[6] == 0x79 && header[7] == 0x70) {
        return FileFormat::MP4;
    }
    
    // 检测 AVI (RIFF header)
    // AVI 格式: 52 49 46 46 ... 41 56 49 20
    if (bytes_read >= 12 &&
        header[0] == 0x52 && header[1] == 0x49 && 
        header[2] == 0x46 && header[3] == 0x46 &&
        header[8] == 0x41 && header[9] == 0x56 && 
        header[10] == 0x49 && header[11] == 0x20) {
        return FileFormat::AVI;
    }
    
    // 检测 H.264 (NAL unit start code)
    // H.264 格式: 00 00 00 01 或 00 00 01
    if (bytes_read >= 4) {
        if ((header[0] == 0x00 && header[1] == 0x00 && 
             header[2] == 0x00 && header[3] == 0x01) ||
            (header[0] == 0x00 && header[1] == 0x00 && header[2] == 0x01)) {
            
            // 进一步检查 NAL unit type (第4或第3字节)
            int nal_byte_idx = (header[3] == 0x01) ? 4 : 3;
            if (bytes_read > nal_byte_idx) {
                unsigned char nal_type = header[nal_byte_idx] & 0x1F;
                // NAL types for H.264: 1-21
                if (nal_type >= 1 && nal_type <= 21) {
                    return FileFormat::H264;
                }
                // NAL types for H.265: 0-40
                if (nal_type <= 40) {
                    return FileFormat::H265;
                }
            }
        }
    }
    
    // 没有检测到已知格式
    return FileFormat::UNKNOWN;
}

ssize_t VideoFile::readFileHeader(unsigned char* header, size_t size) {
    if (fd_ < 0) {
        return -1;
    }
    
    // 保存当前文件位置
    off_t current_pos = lseek(fd_, 0, SEEK_CUR);
    
    // 回到文件开头
    if (lseek(fd_, 0, SEEK_SET) < 0) {
        return -1;
    }
    
    // 读取文件头
    ssize_t bytes_read = read(fd_, header, size);
    
    // 恢复文件位置
    lseek(fd_, current_pos, SEEK_SET);
    
    return bytes_read;
}

bool VideoFile::parseMP4Header() {
    printf("⚠️  MP4 format detected but not yet fully supported\n");
    printf("   Please use a tool to extract raw frames, or provide format info\n");
    return false;
}

bool VideoFile::parseH264Header() {
    printf("⚠️  H.264 format detected but not yet fully supported\n");
    printf("   Please use a tool to extract raw frames, or provide format info\n");
    return false;
}

bool VideoFile::mapFile() {
    if (fd_ < 0) {
        printf("❌ ERROR: Invalid file descriptor\n");
        return false;
    }
    
    if (file_size_ <= 0) {
        printf("❌ ERROR: Invalid file size: %ld\n", file_size_);
        return false;
    }
    
    // 使用 mmap 映射整个文件到进程地址空间
    // PROT_READ: 只读访问
    // MAP_PRIVATE: 私有映射（写时复制，修改不影响原文件）
    mapped_file_ = mmap(NULL, file_size_, 
                        PROT_READ, MAP_PRIVATE, 
                        fd_, 0);
    
    if (mapped_file_ == MAP_FAILED) {
        printf("❌ ERROR: mmap failed: %s\n", strerror(errno));
        mapped_file_ = nullptr;
        return false;
    }
    
    mapped_size_ = file_size_;
    
    printf("🗺️  File mapped to memory: address=%p, size=%zu bytes\n", 
           mapped_file_, mapped_size_);
    
    return true;
}

void VideoFile::unmapFile() {
    if (mapped_file_ != nullptr && mapped_size_ > 0) {
        if (munmap(mapped_file_, mapped_size_) < 0) {
            printf("⚠️  Warning: munmap failed: %s\n", strerror(errno));
        }
        mapped_file_ = nullptr;
        mapped_size_ = 0;
    }
}

