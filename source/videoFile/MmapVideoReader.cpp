#include "../../include/videoFile/MmapVideoReader.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>

// ============ 构造函数 ============

MmapVideoReader::MmapVideoReader()
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
    path_[0] = '\0';
}

MmapVideoReader::~MmapVideoReader() {
    close();
}

// ============ IVideoReader 接口实现 ============

bool MmapVideoReader::open(const char* path) {
    if (is_open_) {
        printf("⚠️  Warning: File already opened, closing previous file\n");
        close();
    }
    
    strncpy(path_, path, MAX_PATH_LENGTH - 1);
    path_[MAX_PATH_LENGTH - 1] = '\0';
    
    printf("📂 Opening video file: %s\n", path);
    printf("   Mode: Auto-detect format\n");
    printf("   Reader: MmapVideoReader (memory-mapped I/O)\n");
    
    fd_ = ::open(path, O_RDONLY);
    if (fd_ < 0) {
        printf("❌ ERROR: Cannot open file: %s\n", strerror(errno));
        return false;
    }
    
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

bool MmapVideoReader::openRaw(const char* path, int width, int height, int bits_per_pixel) {
    if (is_open_) {
        printf("⚠️  Warning: File already opened, closing previous file\n");
        close();
    }
    
    if (width <= 0 || height <= 0 || bits_per_pixel <= 0) {
        printf("❌ ERROR: Invalid parameters\n");
        printf("   width=%d, height=%d, bits_per_pixel=%d\n", 
               width, height, bits_per_pixel);
        return false;
    }
    
    strncpy(path_, path, MAX_PATH_LENGTH - 1);
    path_[MAX_PATH_LENGTH - 1] = '\0';
    width_ = width;
    height_ = height;
    bits_per_pixel_ = bits_per_pixel;
    
    size_t total_bits = (size_t)width_ * height_ * bits_per_pixel_;
    frame_size_ = (total_bits + 7) / 8;
    
    detected_format_ = FileFormat::RAW;
    
    printf("📂 Opening raw video file: %s\n", path);
    printf("   Format: %dx%d, %d bits per pixel\n", 
           width_, height_, bits_per_pixel_);
    printf("   Frame size: %zu bytes\n", frame_size_);
    printf("   Reader: MmapVideoReader (memory-mapped I/O)\n");
    
    fd_ = ::open(path, O_RDONLY);
    if (fd_ < 0) {
        printf("❌ ERROR: Cannot open file: %s\n", strerror(errno));
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
    
    printf("✅ Raw video file opened successfully\n");
    printf("   File size: %ld bytes\n", file_size_);
    printf("   Total frames: %d\n", total_frames_);
    
    return true;
}

void MmapVideoReader::close() {
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
    
    printf("✅ Video file closed: %s\n", path_);
}

bool MmapVideoReader::isOpen() const {
    return is_open_;
}

bool MmapVideoReader::readFrameTo(Buffer& dest_buffer) {
    return readFrameTo(dest_buffer.data(), dest_buffer.size());
}

bool MmapVideoReader::readFrameTo(void* dest_buffer, size_t buffer_size) {
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
    
    if (current_frame_index_ >= total_frames_) {
        printf("⚠️  Warning: Reached end of file\n");
        return false;
    }
    
    size_t frame_offset = (size_t)current_frame_index_ * frame_size_;
    const char* frame_addr = (const char*)mapped_file_ + frame_offset;
    
    memcpy(dest_buffer, frame_addr, frame_size_);
    
    current_frame_index_++;
    return true;
}

bool MmapVideoReader::readFrameAt(int frame_index, Buffer& dest_buffer) {
    return readFrameAt(frame_index, dest_buffer.data(), dest_buffer.size());
}

bool MmapVideoReader::readFrameAt(int frame_index, void* dest_buffer, size_t buffer_size) {
    if (!seek(frame_index)) {
        return false;
    }
    
    return readFrameTo(dest_buffer, buffer_size);
}

bool MmapVideoReader::readFrameAtThreadSafe(int frame_index, void* dest_buffer, size_t buffer_size) const {
    if (!is_open_ || mapped_file_ == nullptr) {
        return false;
    }
    
    if (frame_index < 0 || frame_index >= total_frames_) {
        return false;
    }
    
    if (buffer_size < frame_size_) {
        return false;
    }
    
    size_t frame_offset = (size_t)frame_index * frame_size_;
    const char* frame_addr = (const char*)mapped_file_ + frame_offset;
    
    memcpy(dest_buffer, frame_addr, frame_size_);
    return true;
}

bool MmapVideoReader::seek(int frame_index) {
    if (!is_open_) {
        printf("❌ ERROR: File not opened\n");
        return false;
    }
    
    if (frame_index < 0 || frame_index >= total_frames_) {
        printf("❌ ERROR: Invalid frame index %d (valid: 0-%d)\n",
               frame_index, total_frames_ - 1);
        return false;
    }
    
    current_frame_index_ = frame_index;
    return true;
}

bool MmapVideoReader::seekToBegin() {
    return seek(0);
}

bool MmapVideoReader::seekToEnd() {
    if (!is_open_) {
        printf("❌ ERROR: File not opened\n");
        return false;
    }
    
    current_frame_index_ = total_frames_;
    return true;
}

bool MmapVideoReader::skip(int frame_count) {
    int target_frame = current_frame_index_ + frame_count;
    return seek(target_frame);
}

int MmapVideoReader::getTotalFrames() const {
    return total_frames_;
}

int MmapVideoReader::getCurrentFrameIndex() const {
    return current_frame_index_;
}

size_t MmapVideoReader::getFrameSize() const {
    return frame_size_;
}

long MmapVideoReader::getFileSize() const {
    return file_size_;
}

int MmapVideoReader::getWidth() const {
    return width_;
}

int MmapVideoReader::getHeight() const {
    return height_;
}

int MmapVideoReader::getBytesPerPixel() const {
    return (bits_per_pixel_ + 7) / 8;
}

const char* MmapVideoReader::getPath() const {
    return path_;
}

bool MmapVideoReader::hasMoreFrames() const {
    return current_frame_index_ < total_frames_;
}

bool MmapVideoReader::isAtEnd() const {
    return current_frame_index_ >= total_frames_;
}

const char* MmapVideoReader::getReaderType() const {
    return "MmapVideoReader";
}

// ============ 内部辅助方法 ============

bool MmapVideoReader::validateFile() {
    struct stat st;
    if (fstat(fd_, &st) < 0) {
        printf("❌ ERROR: Cannot get file size: %s\n", strerror(errno));
        return false;
    }
    
    file_size_ = st.st_size;
    
    if (file_size_ == 0) {
        printf("❌ ERROR: File is empty\n");
        return false;
    }
    
    total_frames_ = file_size_ / frame_size_;
    
    if (total_frames_ == 0) {
        printf("❌ ERROR: File too small (size=%ld, frame_size=%zu)\n",
               file_size_, frame_size_);
        return false;
    }
    
    if (file_size_ % frame_size_ != 0) {
        printf("⚠️  Warning: File size (%ld) not aligned to frame size (%zu)\n",
               file_size_, frame_size_);
        printf("   Last frame may be incomplete\n");
    }
    
    return true;
}

MmapVideoReader::FileFormat MmapVideoReader::detectFileFormat() {
    unsigned char header[32];
    ssize_t bytes_read = readFileHeader(header, sizeof(header));
    
    if (bytes_read < 16) {
        printf("⚠️  Warning: Cannot read enough header data\n");
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

ssize_t MmapVideoReader::readFileHeader(unsigned char* header, size_t size) {
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

bool MmapVideoReader::parseMP4Header() {
    printf("⚠️  MP4 format detected but not yet fully supported\n");
    printf("   Please use a tool to extract raw frames, or provide format info\n");
    return false;
}

bool MmapVideoReader::parseH264Header() {
    printf("⚠️  H.264 format detected but not yet fully supported\n");
    printf("   Please use a tool to extract raw frames, or provide format info\n");
    return false;
}

bool MmapVideoReader::mapFile() {
    if (fd_ < 0) {
        printf("❌ ERROR: Invalid file descriptor\n");
        return false;
    }
    
    if (file_size_ <= 0) {
        printf("❌ ERROR: Invalid file size: %ld\n", file_size_);
        return false;
    }
    
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

void MmapVideoReader::unmapFile() {
    if (mapped_file_ != nullptr && mapped_size_ > 0) {
        if (munmap(mapped_file_, mapped_size_) < 0) {
            printf("⚠️  Warning: munmap failed: %s\n", strerror(errno));
        }
        mapped_file_ = nullptr;
        mapped_size_ = 0;
    }
}








