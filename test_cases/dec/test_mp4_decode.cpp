/**
 * Multi-Codec Video Decode Test
 * 
 * 通用视频解码测试程序，支持多种编码格式和灵活配置
 * 
 * 功能：
 * - 支持多种编码格式：H264, H265(HEVC), MJPEG
 * - 使用硬件解码器（h264_taco, hevc_taco, mjpeg_taco）
 * - 可配置分辨率、帧率、线程数
 * - 多线程并行解码，提升性能
 * - 实时显示到 Framebuffer（可选）
 * - 保存解码后的数据用于验证
 * - 性能监控和详细统计报告
 * - 自动验证解码 FPS 是否达标
 * 
 * 编译：
 *   通过 Buildroot 构建系统：
 *     cd /home/zyko/workshop-debian
 *     make components-rebuild
 * 
 *   或手动编译（不推荐）：
 *     g++ -o test_mp4_decode test_mp4_decode.cpp \
 *         -I../../include -L../../build/lib \
 *         -lcomponents -lavformat -lavcodec -lavutil -lpthread -std=c++17
 * 
 * 使用方法：
 *   # H264 解码
 *   ./test_mp4_decode video.mp4
 *   ./test_mp4_decode video.mp4 --codec h264
 *   
 *   # H265 解码
 *   ./test_mp4_decode video.mp4 --codec h265
 *   
 *   # MJPEG 解码
 *   ./test_mp4_decode video.mjpeg --codec mjpeg
 *   
 *   # 自定义参数
 *   ./test_mp4_decode video.mp4 --codec h264 --resolution 1280x720 --fps 30
 *   ./test_mp4_decode video.mp4 --save-frames 100 --max-frames 500
 *   ./test_mp4_decode video.mp4 --no-display --threads 4
 *   ./test_mp4_decode video.mp4 --output /tmp/my.rgb --save-frames -1
 *   
 *   # PSNR 验证（需要FFmpeg）
 *   ./test_mp4_decode video.mp4 --enable-psnr --save-frames -1
 *   ./test_mp4_decode video.mp4 --enable-psnr --min-psnr 35.0 --save-frames -1
 * 
 * 验证解码结果：
 *   ffplay -f rawvideo -pix_fmt argb -s 1920x1080 /tmp/decoded_*.rgb
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/stat.h>
#include <cmath>
#include <chrono>
#include <thread>
#include <atomic>
#include <memory>

// Components 头文件
#include "productionline/VideoProductionLine.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include "productionline/io/BufferWriter.hpp"
#include "buffer/bufferpool/BufferPool.hpp"
#include "buffer/bufferpool/BufferPoolRegistry.hpp"
#include "display/LinuxFramebufferDevice.hpp"
#include "monitor/PerformanceMonitor.hpp"
#include "common/Logger.hpp"

// FFmpeg 头文件（用于 av_get_pix_fmt_name）
extern "C" {
#include <libavutil/pixdesc.h>
}

// 全局变量
static std::atomic<bool> g_running(true);

// 信号处理函数
void signal_handler(int signum) {
    printf("\n[Signal] Caught signal %d, stopping...\n", signum);
    g_running = false;
}

/**
 * @brief 将 Buffer 中的帧数据转换为 YUV420P 并写入文件
 * @param fp 输出文件指针
 * @param buffer Buffer对象（可能包含NV12或YUV420P数据）
 * @param width 图像宽度
 * @param height 图像高度
 * @param is_software_decoder 是否为软件解码器（用于正确计算stride）
 * @return true 成功，false 失败
 * 
 * @note 自动检测像素格式：
 *       - 如果已经是 YUV420P，直接写入
 *       - 如果是 NV12，转换为 YUV420P 后写入
 * @note NV12 格式：数据连续存储，[Y plane] [UV interleaved]
 * @note YUV420P 格式：三个独立平面 [Y plane] [U plane] [V plane]
 * @note 软件解码器即使使用DMA buffer，stride通常也是width（未对齐）
 * @note 硬件解码器通常要求stride对齐到128字节边界
 */
bool write_nv12_as_yuv420p(FILE* fp, Buffer* buffer, int width, int height, bool is_software_decoder = false) {
    if (!fp || !buffer) {
        return false;
    }
    
    AVFrame* frame = buffer->getAVFrame();
    if (!frame) {
        LOG_ERROR("[ERROR] AVFrame not available");
        return false;
    }
    
    // 检测像素格式
    // 主要通过data指针布局判断：
    // YUV420P: data[0]=Y, data[1]=U, data[2]=V (三个独立指针，U和V指针不同)
    // NV12: data[0]=Y, data[1]=UV交错 (通常只有两个data指针，或data[2]为空)
    bool is_yuv420p = false;
    bool is_nv12 = false;
    
    uint8_t* base_addr = (uint8_t*)buffer->getVirtualAddress();
    
    // 详细日志（前几次调用记录详细信息）
    // 使用两个独立的计数器：一个用于硬件解码器，一个用于软件解码器
    static int hw_call_count = 0;
    static int sw_call_count = 0;
    static bool hw_detailed_logged = false;
    static bool sw_detailed_logged = false;
    
    // 使用传入的参数来判断是硬件还是软件解码器（更可靠）
    // 如果参数未指定，则通过AVFrame特征判断
    bool is_hardware_decoder = !is_software_decoder;
    
    // 如果未指定参数，尝试通过AVFrame特征判断
    if (!is_software_decoder) {
        if (frame->format == -1 && frame->linesize[0] == 0 && !frame->data[0] && base_addr) {
            // 典型的硬件解码器特征：format=-1, linesize=0, data指针为nil
            is_hardware_decoder = true;
        } else if (frame->data[0] && frame->data[0] != base_addr && frame->linesize[0] > 0) {
            // 典型的软件解码器特征：data指针有效且不等于base_addr，linesize>0
            is_hardware_decoder = false;
        } else if (!frame->data[0] && base_addr) {
            // 如果data指针为nil但有base_addr，可能是硬件解码器
            is_hardware_decoder = true;
        } else {
            // 默认：如果有有效的data指针，假设是软件解码器
            is_hardware_decoder = !frame->data[0];
        }
    }
    
    if (is_hardware_decoder) {
        hw_call_count++;
    } else {
        sw_call_count++;
    }
    
    // 强制输出前几次的详细日志（硬件和软件各3次）
    bool should_log_detail = false;
    if (is_hardware_decoder) {
        should_log_detail = (hw_call_count <= 3) || (!hw_detailed_logged && hw_call_count == 1);
    } else {
        should_log_detail = (sw_call_count <= 3) || (!sw_detailed_logged && sw_call_count == 1);
        // 软件解码器：强制输出前3次，确保能看到格式信息
        if (sw_call_count <= 3) {
            should_log_detail = true;
        }
    }
    
    if (should_log_detail) {
        int current_count = is_hardware_decoder ? hw_call_count : sw_call_count;
        LOG_INFO_FMT("[DEBUG] Frame #%d (%s decoder): AVFrame format=%d", 
                    current_count, is_hardware_decoder ? "hardware" : "software", frame->format);
        LOG_INFO_FMT("[DEBUG]   data[0]=%p, data[1]=%p, data[2]=%p, base_addr=%p",
                    frame->data[0], frame->data[1], frame->data[2], base_addr);
        LOG_INFO_FMT("[DEBUG]   linesize[0]=%d, linesize[1]=%d, linesize[2]=%d",
                    frame->linesize[0], frame->linesize[1], frame->linesize[2]);
    }
    
    // 方法1：检查format字段（最可靠）
    // AV_PIX_FMT_YUV420P = 0, AV_PIX_FMT_NV12 = 23
    if (frame->format == 0) {  // AV_PIX_FMT_YUV420P
        is_yuv420p = true;
        if (should_log_detail) {
            LOG_INFO("[DEBUG]   Detected YUV420P by format field (format=0)");
        }
    } else if (frame->format == 23) {  // AV_PIX_FMT_NV12
        is_nv12 = true;
        if (should_log_detail) {
            LOG_INFO("[DEBUG]   Detected NV12 by format field (format=23)");
        }
    }
    
    // 方法2：通过data指针布局判断（如果format字段不可靠）
    if (!is_yuv420p && !is_nv12) {
        // YUV420P: 必须有三个独立的data指针，且U和V指针不同
        if (frame->data[0] && frame->data[1] && frame->data[2] && 
            frame->data[1] != frame->data[2] &&
            frame->linesize[1] > 0 && frame->linesize[2] > 0 &&
            frame->linesize[1] == frame->linesize[2]) {  // U和V的stride应该相同
            is_yuv420p = true;
            if (should_log_detail) {
                LOG_INFO("[DEBUG]   Detected YUV420P by data pointer layout (3 independent planes)");
            }
        }
        // NV12: 通常只有两个data指针，或data[2]为空/与data[1]相同
        else if (frame->data[0] && frame->data[1] && 
                 (!frame->data[2] || frame->data[1] == frame->data[2])) {
            is_nv12 = true;
            if (should_log_detail) {
                LOG_INFO("[DEBUG]   Detected NV12 by data pointer layout (2 planes, UV interleaved)");
            }
        }
    }
    
    // 方法3：如果仍无法确定，使用启发式规则
    if (!is_yuv420p && !is_nv12) {
        if (base_addr && frame->data[0] == base_addr) {
            // 如果data[0]指向连续内存的起始地址，可能是NV12（硬件解码器常见）
            is_nv12 = true;
            if (should_log_detail) {
                LOG_INFO("[DEBUG]   Detected NV12 by heuristic (data[0] == base_addr)");
            }
        } else if (frame->data[0] && frame->data[1] && frame->data[2]) {
            // 否则如果有三个data指针，假设是YUV420P
            is_yuv420p = true;
            if (should_log_detail) {
                LOG_INFO("[DEBUG]   Detected YUV420P by heuristic (3 data pointers)");
            }
        } else {
            // 默认假设NV12（硬件解码器常见）
            is_nv12 = true;
            if (should_log_detail) {
                LOG_INFO("[DEBUG]   Default to NV12 (hardware decoder common)");
            }
        }
    }
    
    if (should_log_detail) {
        LOG_INFO_FMT("[DEBUG]   Final decision: %s (%s decoder)", 
                    is_yuv420p ? "YUV420P" : "NV12",
                    is_hardware_decoder ? "hardware" : "software");
        if (is_hardware_decoder) {
            hw_detailed_logged = true;
        } else {
            sw_detailed_logged = true;
        }
    }
    
    // 获取 stride 信息
    // 如果linesize为0（DMA buffer常见情况），需要手动计算对齐后的stride
    // 硬件解码器通常要求stride对齐到128字节边界
    int y_stride, u_stride, v_stride, uv_stride;
    
    if (frame->linesize[0] > 0) {
        y_stride = frame->linesize[0];
    } else {
        // linesize=0：需要手动计算stride
        // ⚠️ 关键发现：两个解码器都使用DMA buffer（format=-1, linesize=0, data=nil）
        // 对于DMA buffer，stride通常需要对齐到128字节边界（硬件要求）
        // 即使软件解码器也使用DMA buffer，它的stride也需要对齐
        y_stride = ((width + 127) / 128) * 128;  // 对齐到128字节 = 384
        if (should_log_detail) {
            int current_count = is_hardware_decoder ? hw_call_count : sw_call_count;
            size_t buffer_size = buffer->getUsedSize();
            size_t calculated_size = y_stride * height + y_stride * (height / 2);  // Y + UV
            if (current_count <= 2) {
                LOG_INFO_FMT("[DEBUG]   Calculated Y stride: %d (width=%d, %s decoder, DMA buffer aligned to 128)", 
                            y_stride, width, is_hardware_decoder ? "hardware" : "software");
                LOG_INFO_FMT("[DEBUG]   Buffer size: %zu bytes, Calculated NV12 size: %zu bytes", 
                            buffer_size, calculated_size);
            }
        }
    }
    
    if (frame->linesize[1] > 0) {
        uv_stride = frame->linesize[1];
        u_stride = frame->linesize[1];
    } else {
        // NV12的UV stride通常是Y stride（因为UV是交错的，每两个字节是一个UV对）
        // 对于YUV420P，U和V的stride通常也需要对齐
        if (is_yuv420p) {
            // DMA buffer：U和V的stride也需要对齐到128字节
            u_stride = ((width / 2 + 127) / 128) * 128;
            v_stride = u_stride;
        } else {
            // NV12: UV stride = Y stride（已经对齐）
            uv_stride = y_stride;
        }
        if (should_log_detail) {
            int current_count = is_hardware_decoder ? hw_call_count : sw_call_count;
            if (current_count <= 2) {
                LOG_INFO_FMT("[DEBUG]   Calculated UV stride: %d (for %s)", 
                            is_yuv420p ? u_stride : uv_stride, is_yuv420p ? "YUV420P" : "NV12");
            }
        }
    }
    
    if (frame->linesize[2] > 0) {
        v_stride = frame->linesize[2];
    } else if (is_yuv420p && frame->linesize[1] == 0) {
        // 已经在上面计算了
    } else {
        v_stride = u_stride;  // 默认与U stride相同
    }
    
    // ========== 情况1：已经是 YUV420P，直接写入 ==========
    if (is_yuv420p) {
        // YUV420P格式：data[0]=Y, data[1]=U, data[2]=V
        uint8_t* y_plane = frame->data[0];
        uint8_t* u_plane = frame->data[1];
        uint8_t* v_plane = frame->data[2];
        
        if (!y_plane || !u_plane || !v_plane) {
            LOG_ERROR("[ERROR] YUV420P data pointers invalid");
            return false;
        }
        
        // 写入 Y 平面
        for (int row = 0; row < height; row++) {
            if (fwrite(y_plane + row * y_stride, 1, width, fp) != (size_t)width) {
                LOG_ERROR_FMT("[ERROR] Failed to write Y plane row %d", row);
                return false;
            }
        }
        
        // 写入 U 平面
        int uv_width = width / 2;
        int uv_height = height / 2;
        for (int row = 0; row < uv_height; row++) {
            if (fwrite(u_plane + row * u_stride, 1, uv_width, fp) != (size_t)uv_width) {
                LOG_ERROR_FMT("[ERROR] Failed to write U plane row %d", row);
                return false;
            }
        }
        
        // 写入 V 平面
        for (int row = 0; row < uv_height; row++) {
            if (fwrite(v_plane + row * v_stride, 1, uv_width, fp) != (size_t)uv_width) {
                LOG_ERROR_FMT("[ERROR] Failed to write V plane row %d", row);
                return false;
            }
        }
        
        return true;
    }
    
    // ========== 情况2：NV12 格式，需要转换 ==========
    // base_addr 已经在函数开头获取，这里直接使用
    if (!base_addr) {
        // 如果没有连续内存，尝试使用AVFrame的data指针
        if (frame->data[0] && frame->data[1]) {
            // NV12: data[0]=Y, data[1]=UV交错
            uint8_t* y_plane = frame->data[0];
            uint8_t* uv_plane = frame->data[1];
            
            // 写入 Y 平面
            for (int row = 0; row < height; row++) {
                if (fwrite(y_plane + row * y_stride, 1, width, fp) != (size_t)width) {
                    LOG_ERROR_FMT("[ERROR] Failed to write Y plane row %d", row);
                    return false;
                }
            }
            
            // 分离 UV 交错数据
            int uv_width = width / 2;
            int uv_height = height / 2;
            size_t uv_plane_size = uv_width * uv_height;
            
            uint8_t* u_buffer = new uint8_t[uv_plane_size];
            uint8_t* v_buffer = new uint8_t[uv_plane_size];
            
            for (int row = 0; row < uv_height; row++) {
                uint8_t* uv_row = uv_plane + row * uv_stride;
                for (int col = 0; col < uv_width; col++) {
                    u_buffer[row * uv_width + col] = uv_row[col * 2];
                    v_buffer[row * uv_width + col] = uv_row[col * 2 + 1];
                }
            }
            
            bool success = true;
            if (fwrite(u_buffer, 1, uv_plane_size, fp) != uv_plane_size) {
                LOG_ERROR("[ERROR] Failed to write U plane");
                success = false;
            }
            if (success && fwrite(v_buffer, 1, uv_plane_size, fp) != uv_plane_size) {
                LOG_ERROR("[ERROR] Failed to write V plane");
                success = false;
            }
            
            delete[] u_buffer;
            delete[] v_buffer;
            return success;
        } else {
            LOG_ERROR("[ERROR] No virtual address or AVFrame data available");
            return false;
        }
    }
    
    // NV12 内存布局（连续内存）：
    // - Y 平面：从 base_addr 开始，每行 y_stride 字节
    // - UV 平面：在 Y 平面之后，每行 uv_stride 字节
    uint8_t* y_plane = base_addr;
    size_t y_stride_total = y_stride * height;
    uint8_t* uv_plane = base_addr + y_stride_total;
    
    // 1. 写入 Y 平面（按行读取，跳过 padding）
    for (int row = 0; row < height; row++) {
        uint8_t* y_row = y_plane + row * y_stride;
        if (fwrite(y_row, 1, width, fp) != (size_t)width) {
            LOG_ERROR_FMT("[ERROR] Failed to write Y plane row %d", row);
            return false;
        }
    }
    
    // 2. 分离 UV 交错数据，写入 U 平面和 V 平面
    int uv_width = width / 2;
    int uv_height = height / 2;
    size_t uv_plane_size = uv_width * uv_height;
    
    uint8_t* u_buffer = new uint8_t[uv_plane_size];
    uint8_t* v_buffer = new uint8_t[uv_plane_size];
    
    // 按行分离交错的 UV 数据（考虑 stride）
    // NV12: UVUVUVUV... -> U: UUUU..., V: VVVV...
    for (int row = 0; row < uv_height; row++) {
        uint8_t* uv_row = uv_plane + row * uv_stride;
        for (int col = 0; col < uv_width; col++) {
            u_buffer[row * uv_width + col] = uv_row[col * 2];      // U 在偶数位置
            v_buffer[row * uv_width + col] = uv_row[col * 2 + 1];  // V 在奇数位置
        }
    }
    
    // 写入 U 平面
    bool success = true;
    if (fwrite(u_buffer, 1, uv_plane_size, fp) != uv_plane_size) {
        LOG_ERROR("[ERROR] Failed to write U plane");
        success = false;
    }
    
    // 写入 V 平面
    if (success && fwrite(v_buffer, 1, uv_plane_size, fp) != uv_plane_size) {
        LOG_ERROR("[ERROR] Failed to write V plane");
        success = false;
    }
    
    delete[] u_buffer;
    delete[] v_buffer;
    
    return success;
}

/**
 * @brief 手动计算两个YUV420P帧之间的PSNR
 * @param frame1 第一帧数据
 * @param frame2 第二帧数据
 * @param width 视频宽度
 * @param height 视频高度
 * @param psnr_y 输出：Y平面PSNR
 * @param psnr_u 输出：U平面PSNR
 * @param psnr_v 输出：V平面PSNR
 * @param psnr_avg 输出：平均PSNR
 * @return true 计算成功，false 计算失败
 */
bool calculate_psnr_manual(const uint8_t* frame1, const uint8_t* frame2, 
                           int width, int height,
                           double& psnr_y, double& psnr_u, double& psnr_v, double& psnr_avg) {
    // YUV420P格式：Y平面是width*height，U和V平面各是width*height/4
    size_t y_size = width * height;
    size_t uv_size = (width / 2) * (height / 2);
    
    // 计算Y平面的MSE
    double mse_y = 0.0;
    for (size_t i = 0; i < y_size; i++) {
        double diff = (double)frame1[i] - (double)frame2[i];
        mse_y += diff * diff;
    }
    mse_y /= y_size;
    
    // 计算U平面的MSE
    const uint8_t* u1 = frame1 + y_size;
    const uint8_t* u2 = frame2 + y_size;
    double mse_u = 0.0;
    for (size_t i = 0; i < uv_size; i++) {
        double diff = (double)u1[i] - (double)u2[i];
        mse_u += diff * diff;
    }
    mse_u /= uv_size;
    
    // 计算V平面的MSE
    const uint8_t* v1 = frame1 + y_size + uv_size;
    const uint8_t* v2 = frame2 + y_size + uv_size;
    double mse_v = 0.0;
    for (size_t i = 0; i < uv_size; i++) {
        double diff = (double)v1[i] - (double)v2[i];
        mse_v += diff * diff;
    }
    mse_v /= uv_size;
    
    // 将MSE转换为PSNR（单位：dB）
    // PSNR = 10 * log10(MAX^2 / MSE)，对于8位数据MAX=255
    const double MAX = 255.0;
    
    if (mse_y > 0.0) {
        psnr_y = 10.0 * log10((MAX * MAX) / mse_y);
    } else {
        psnr_y = 100.0;  // 完全相同
    }
    
    if (mse_u > 0.0) {
        psnr_u = 10.0 * log10((MAX * MAX) / mse_u);
    } else {
        psnr_u = 100.0;
    }
    
    if (mse_v > 0.0) {
        psnr_v = 10.0 * log10((MAX * MAX) / mse_v);
    } else {
        psnr_v = 100.0;
    }
    
    // 计算加权平均PSNR（Y权重更高，因为Y包含更多像素）
    double mse_avg = (mse_y * y_size + mse_u * uv_size + mse_v * uv_size) / (y_size + uv_size + uv_size);
    if (mse_avg > 0.0) {
        psnr_avg = 10.0 * log10((MAX * MAX) / mse_avg);
    } else {
        psnr_avg = 100.0;
    }
    
    return true;
}

/**
 * @brief 使用FFmpeg psnr滤镜进行PSNR验证（采样处理，逐帧加载释放）
 * @param hw_yuv_path 硬件解码输出的YUV420P文件
 * @param source_video 原始视频文件
 * @param width 视频宽度
 * @param height 视频高度
 * @param frame_count 帧数
 * @param min_psnr 最小PSNR要求
 * @return true PSNR达标，false 不达标
 * 
 * @note 采样策略以避免内存不足（每隔10帧采样1帧）
 * @note 使用FFmpeg的psnr滤镜计算PSNR，更加精确可靠
 * @note 逐帧提取、计算、清理，不会将所有帧同时加载到内存
 */
bool validate_psnr_streaming(const char* hw_yuv_path, const char* source_video,
                              int width, int height, int frame_count, double min_psnr) {
    LOG_INFO("");
    LOG_INFO("╔═══════════════════════════════════════════════════════╗");
    LOG_INFO("║  PSNR Validation (Frame Sampling Mode)                ║");
    LOG_INFO("╚═══════════════════════════════════════════════════════╝");
    LOG_INFO_FMT("Hardware YUV: %s", hw_yuv_path);
    LOG_INFO_FMT("Source video: %s", source_video);
    LOG_INFO_FMT("Resolution: %dx%d, Frames: %d", width, height, frame_count);
    LOG_INFO_FMT("Minimum PSNR requirement: %.2f dB", min_psnr);
    LOG_INFO("");
    
    // 生成完整的参考YUV文件（使用组件的软件解码器）
    char ref_yuv_path[256];
    snprintf(ref_yuv_path, sizeof(ref_yuv_path), "/tmp/ref_%dx%d_%d.yuv", width, height, getpid());
    
    LOG_INFO("[1/3] Generating reference YUV with software decoder (component)...");
    LOG_INFO_FMT("   Source: %s", source_video);
    LOG_INFO_FMT("   Output: %s", ref_yuv_path);
    LOG_INFO_FMT("   Frames: %d", frame_count);
    
    // 创建软件解码生产者
    VideoProductionLine sw_producer(false, 1, false);  // loop=false, thread_count=1
    
    // 配置软件解码器
    auto sw_workerConfig = WorkerConfigBuilder()
        .setFileConfig(
            FileConfigBuilder()
                .setFilePath(source_video)
                .build()
        )
        .setDisplayConfig(
            DisplayConfigBuilder()
                .setDisplayResolution(width, height)
                .setBitsPerPixel(12)  // YUV420P/NV12
                .build()
        )
        .setDecoderConfig(
            DecoderConfigBuilder()
                .useSoftware()  // ⭐ 使用软件解码器
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
    
    // 启动软件解码
    bool sw_started = sw_producer.start(sw_workerConfig);
    if (!sw_started) {
        LOG_ERROR("❌ Failed to start software decoder");
        return false;
    }
    
    // 获取软件解码的 BufferPool
    uint64_t sw_pool_id = sw_producer.getWorkingBufferPoolId();
    if (sw_pool_id == 0) {
        LOG_ERROR("❌ Software decoder BufferPool not available");
        sw_producer.stop();
        return false;
    }
    
    auto sw_pool_weak = BufferPoolRegistry::getInstance().getPool(sw_pool_id);
    auto sw_pool_sptr = sw_pool_weak.lock();
    if (!sw_pool_sptr) {
        LOG_ERROR("❌ Software decoder BufferPool not found");
        sw_producer.stop();
        return false;
    }
    
    LOG_INFO_FMT("   BufferPool: '%s' (ID: %lu)", 
                sw_pool_sptr->getName().c_str(), sw_pool_id);
    
    // 打开输出文件
    FILE* sw_yuv_fp = fopen(ref_yuv_path, "wb");
    if (!sw_yuv_fp) {
        LOG_ERROR_FMT("❌ Failed to open output file: %s", ref_yuv_path);
        sw_producer.stop();
        return false;
    }
    
    // 消费帧并保存到文件
    int sw_frame_count = 0;
    
    LOG_INFO("   Decoding frames...");
    while (sw_frame_count < frame_count && sw_producer.isRunning()) {
        Buffer* buffer = sw_pool_sptr->acquireFilled(true, 1000);  // 1秒超时
        
        if (buffer == nullptr) {
            // 超时，检查是否还在运行
            if (!sw_producer.isRunning()) {
                LOG_INFO("   Software decoder stopped naturally");
                break;
            }
            continue;
        }
        
        // 保存帧数据（NV12 -> YUV420P 转换）
        // ⚠️ 软件解码器可能也输出 NV12 格式（尤其是FFmpeg 4.x+），需要统一转换
        static int sw_write_count = 0;
        sw_write_count++;
        if (sw_write_count <= 3) {
            AVFrame* sw_frame = buffer->getAVFrame();
            LOG_INFO_FMT("[DEBUG] Software decoder write #%d: format=%d, linesize[0]=%d, data[0]=%p", 
                        sw_write_count, sw_frame ? sw_frame->format : -999, 
                        sw_frame ? sw_frame->linesize[0] : -1,
                        sw_frame ? sw_frame->data[0] : nullptr);
        }
        // ⭐ 明确标识这是软件解码器，使用未对齐的stride
        if (write_nv12_as_yuv420p(sw_yuv_fp, buffer, width, height, true)) {
            sw_frame_count++;
        } else {
            LOG_WARN("Failed to write software decoded frame");
        }
        
        sw_pool_sptr->releaseFilled(buffer);  // ⭐ 消费者归还：LOCKED_BY_CONSUMER → IDLE → free queue
        
        // 显示进度
        if (sw_frame_count % 50 == 0 || sw_frame_count == frame_count) {
            LOG_INFO_FMT("   Progress: %d/%d frames (%.1f fps)", 
                        sw_frame_count, frame_count, sw_producer.getAverageFPS());
        }
    }
    
    fclose(sw_yuv_fp);
    
    // 停止软件解码
    sw_producer.stop();
    
    if (sw_frame_count < frame_count) {
        LOG_WARN_FMT("⚠️  Software decoder produced fewer frames than expected: %d/%d", 
                    sw_frame_count, frame_count);
        // 更新实际帧数
        frame_count = sw_frame_count;
    }
    
    LOG_INFO_FMT("✅ Reference YUV generated: %s (%d frames)", ref_yuv_path, sw_frame_count);
    
    // 计算PSNR（使用手动C++计算，处理每一帧，避免FFmpeg滤镜的内存问题）
    LOG_INFO("[2/3] Calculating PSNR with manual C++ computation (all frames)...");
    const int sample_interval = 1;  // 处理每一帧
    int num_samples = (frame_count + sample_interval - 1) / sample_interval;
    
    double total_psnr_y = 0.0, total_psnr_u = 0.0, total_psnr_v = 0.0, total_psnr_avg = 0.0;
    int valid_samples = 0;
    
    size_t frame_size = width * height * 3 / 2;  // YUV420P: 1.5 bytes per pixel
    
    for (int sample = 0; sample < num_samples; sample++) {
        int frame_idx = sample * sample_interval;
        if (frame_idx >= frame_count) break;
        
        if (sample % 20 == 0 || sample == num_samples - 1) {
            LOG_INFO_FMT("   Progress: %d/%d frames", 
                        sample + 1, num_samples);
        }
        
        // 创建临时单帧文件
        char hw_frame_path[256], ref_frame_path[256];
        snprintf(hw_frame_path, sizeof(hw_frame_path), "/tmp/hw_frame_%d_%d.yuv", getpid(), sample);
        snprintf(ref_frame_path, sizeof(ref_frame_path), "/tmp/ref_frame_%d_%d.yuv", getpid(), sample);
        
        // 提取硬件输出单帧
        char cmd_extract_hw[512];
        snprintf(cmd_extract_hw, sizeof(cmd_extract_hw),
            "dd if='%s' of='%s' bs=%zu skip=%d count=1 2>/dev/null",
            hw_yuv_path, hw_frame_path, frame_size, frame_idx);
        system(cmd_extract_hw);
        
        // 提取参考单帧
        char cmd_extract_ref[512];
        snprintf(cmd_extract_ref, sizeof(cmd_extract_ref),
            "dd if='%s' of='%s' bs=%zu skip=%d count=1 2>/dev/null",
            ref_yuv_path, ref_frame_path, frame_size, frame_idx);
        system(cmd_extract_ref);
        
        // 使用手动C++计算PSNR（读取两帧到内存，直接计算，避免FFmpeg滤镜的内存问题）
        uint8_t* hw_frame = new uint8_t[frame_size];
        uint8_t* ref_frame = new uint8_t[frame_size];
        
        bool calc_success = false;
        double psnr_y = 0.0, psnr_u = 0.0, psnr_v = 0.0, psnr_avg = 0.0;
        
        // 读取硬件帧
        FILE* hw_file = fopen(hw_frame_path, "rb");
        if (hw_file) {
            size_t read_hw = fread(hw_frame, 1, frame_size, hw_file);
            fclose(hw_file);
            
            // 读取参考帧
            FILE* ref_file = fopen(ref_frame_path, "rb");
            if (ref_file) {
                size_t read_ref = fread(ref_frame, 1, frame_size, ref_file);
                fclose(ref_file);
                
                // 两帧都成功读取，计算PSNR
                if (read_hw == frame_size && read_ref == frame_size) {
                    calc_success = calculate_psnr_manual(hw_frame, ref_frame, 
                                                         width, height,
                                                         psnr_y, psnr_u, psnr_v, psnr_avg);
                }
            }
        }
        
        // 释放帧缓冲区
        delete[] hw_frame;
        delete[] ref_frame;
        
        if (calc_success) {
            total_psnr_y += psnr_y;
            total_psnr_u += psnr_u;
            total_psnr_v += psnr_v;
            total_psnr_avg += psnr_avg;
            valid_samples++;
        } else {
            // 计算失败，记录警告
            if (valid_samples == 0 && sample == 0) {
                LOG_WARN_FMT("      Failed to calculate PSNR for frame %d", frame_idx);
            }
        }
        
        // 清理单帧临时文件
        unlink(hw_frame_path);
        unlink(ref_frame_path);
    }
    
    // 清理参考文件
    unlink(ref_yuv_path);
    
    LOG_INFO("");
    LOG_INFO("[3/3] Computing overall PSNR...");
    
    if (valid_samples == 0) {
        LOG_ERROR("❌ Failed to calculate PSNR for any frame");
        return false;
    }
    
    // 计算平均PSNR
    double avg_psnr_y = total_psnr_y / valid_samples;
    double avg_psnr_u = total_psnr_u / valid_samples;
    double avg_psnr_v = total_psnr_v / valid_samples;
    double avg_psnr_avg = total_psnr_avg / valid_samples;
    
    LOG_INFO("");
    LOG_INFO("--- Overall PSNR Results ---");
    LOG_INFO_FMT("Samples processed: %d/%d (every %d frames)", valid_samples, num_samples, sample_interval);
    LOG_INFO_FMT("PSNR Y (luma):       %.2f dB", avg_psnr_y);
    LOG_INFO_FMT("PSNR U (chroma):     %.2f dB", avg_psnr_u);
    LOG_INFO_FMT("PSNR V (chroma):     %.2f dB", avg_psnr_v);
    LOG_INFO_FMT("PSNR Average:        %.2f dB", avg_psnr_avg);
    LOG_INFO("");
    
    if (avg_psnr_avg >= min_psnr) {
        LOG_INFO_FMT("✅ PSNR PASS: Average PSNR (%.2f dB) >= Requirement (%.2f dB)",
                    avg_psnr_avg, min_psnr);
        return true;
    } else {
        LOG_WARN_FMT("❌ PSNR FAIL: Average PSNR (%.2f dB) < Requirement (%.2f dB)",
                    avg_psnr_avg, min_psnr);
        return false;
    }
}


// 编码格式枚举
enum class CodecType {
    H264,
    H265,
    MJPEG,
    CUSTOM  // 自定义解码器
};

// 测试配置结构体
struct TestConfig {
    const char* video_path;
    CodecType codec_type;
    const char* decoder_name;
    const char* output_path;
    const char* profile;         // H264 Profile: baseline, main, high
    bool enable_display;
    bool enable_psnr;       // 启用PSNR验证
    double min_psnr;        // 最小PSNR要求（dB）
    int save_frames;        // 保存多少帧，0=不保存，-1=全部保存
    int max_frames;         // 最大处理帧数，-1=全部处理
    int threads;            // 生产线线程数
    int width;              // 视频宽度
    int height;             // 视频高度
    int fps;                // 目标帧率（用于性能验证）
    bool auto_test;         // 自动测试模式
    
    TestConfig() 
        : video_path(nullptr)
        , codec_type(CodecType::H264)
        , decoder_name(nullptr)
        , output_path(nullptr)
        , profile("high")
        , enable_display(true)
        , enable_psnr(false)
        , min_psnr(30.0)
        , save_frames(300)
        , max_frames(600)
        , threads(2)
        , width(1920)
        , height(1080)
        , fps(60)
        , auto_test(false)
    {}
    
    // 获取实际使用的解码器名称
    const char* getDecoderName() const {
        // 如果用户指定了自定义解码器，优先使用
        if (decoder_name != nullptr) {
            return decoder_name;
        }
        
        // 否则根据 codec_type 自动选择
        switch (codec_type) {
            case CodecType::H264:
                return "h264_taco";
            case CodecType::H265:
                return "hevc_taco";
            case CodecType::MJPEG:
                return "mjpeg_taco";
            default:
                return "h264_taco";
        }
    }
    
    // 获取编码格式名称（用于显示）
    const char* getCodecName() const {
        if (decoder_name != nullptr) {
            return "CUSTOM";
        }
        
        switch (codec_type) {
            case CodecType::H264:
                return "H264";
            case CodecType::H265:
                return "H265";
            case CodecType::MJPEG:
                return "MJPEG";
            default:
                return "UNKNOWN";
        }
    }
};

// 解析命令行参数
TestConfig parse_arguments(int argc, char* argv[]) {
    TestConfig config;
    
    if (argc < 2) {
        printf("Usage: %s <video_file> [options]\n", argv[0]);
        printf("\n");
        printf("Options:\n");
        printf("  --codec TYPE         Codec type: h264, h265, mjpeg (default: h264)\n");
        printf("  --profile PROFILE    H264 profile: baseline, main, high (default: high)\n");
        printf("  --decoder NAME       Custom decoder name (overrides --codec)\n");
        printf("  --output PATH        Output file path (default: auto-generated)\n");
        printf("  --no-display         Disable display output\n");
        printf("  --enable-psnr        Enable PSNR validation (requires FFmpeg)\n");
        printf("  --min-psnr VALUE     Minimum PSNR requirement in dB (default: 30.0)\n");
        printf("  --save-frames N      Save N frames (0=none, -1=all, default: 300)\n");
        printf("  --max-frames N       Max frames to process (-1=all, default: 600)\n");
        printf("  --threads N          Producer threads (default: 2)\n");
        printf("  --resolution WxH     Video resolution (default: 1920x1080)\n");
        printf("  --fps N              Target FPS for validation (default: 60)\n");
        printf("  --auto-test          Run all test cases automatically\n");
        printf("  --all-tests          Alias for --auto-test\n");
        printf("\n");
        printf("Examples:\n");
        printf("  %s video.mp4\n", argv[0]);
        printf("  %s video.mp4 --codec h264 --profile high\n", argv[0]);
        printf("  %s video.mp4 --codec h265\n", argv[0]);
        printf("  %s video.mp4 --codec mjpeg --resolution 1280x720\n", argv[0]);
        printf("  %s video.mp4 --save-frames 100 --max-frames 500\n", argv[0]);
        printf("  %s video.mp4 --no-display --decoder h264_taco\n", argv[0]);
        printf("  %s video.mp4 --resolution 1280x720 --fps 30 --threads 4\n", argv[0]);
        printf("  %s video.mp4 --output /tmp/my_output.yuv --save-frames -1\n", argv[0]);
        printf("  %s video.mp4 --enable-psnr --save-frames -1\n", argv[0]);
        printf("  %s video.mp4 --enable-psnr --min-psnr 35.0 --save-frames -1\n", argv[0]);
        printf("  %s --auto-test\n", argv[0]);
        printf("  %s --auto-test --no-display --threads 4\n", argv[0]);
        printf("\n");
        exit(1);
    }
    
    // 检查是否是自动测试模式
    bool is_auto_test = false;
    int video_file_index = -1;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--auto-test") == 0 || strcmp(argv[i], "--all-tests") == 0) {
            is_auto_test = true;
            config.auto_test = true;
        } else if (argv[i][0] != '-' && video_file_index == -1) {
            // 第一个非选项参数作为视频文件
            video_file_index = i;
        }
    }
    
    if (is_auto_test) {
        // 自动测试模式：不需要指定视频文件（或可选）
        config.video_path = nullptr;
    } else {
        // 普通模式：需要指定视频文件
        if (video_file_index == -1) {
            fprintf(stderr, "Error: Video file not specified\n");
            fprintf(stderr, "Use --auto-test to run all test cases automatically\n");
            exit(1);
        }
        config.video_path = argv[video_file_index];
    }
    
    // 解析选项（跳过视频文件参数）
    int start_index = is_auto_test ? 1 : 2;  // 自动测试模式从1开始，普通模式从2开始
    for (int i = start_index; i < argc; i++) {
        // 跳过视频文件参数
        if (i == video_file_index) {
            continue;
        }
        if (strcmp(argv[i], "--codec") == 0 && i + 1 < argc) {
            const char* codec = argv[++i];
            if (strcasecmp(codec, "h264") == 0) {
                config.codec_type = CodecType::H264;
            } else if (strcasecmp(codec, "h265") == 0 || strcasecmp(codec, "hevc") == 0) {
                config.codec_type = CodecType::H265;
            } else if (strcasecmp(codec, "mjpeg") == 0) {
                config.codec_type = CodecType::MJPEG;
            } else {
                fprintf(stderr, "Unknown codec type: %s\n", codec);
                fprintf(stderr, "Supported codecs: h264, h265, mjpeg\n");
                exit(1);
            }
        } else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            config.profile = argv[++i];
        } else if (strcmp(argv[i], "--decoder") == 0 && i + 1 < argc) {
            config.decoder_name = argv[++i];
            config.codec_type = CodecType::CUSTOM;
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            config.output_path = argv[++i];
        } else if (strcmp(argv[i], "--no-display") == 0) {
            config.enable_display = false;
        } else if (strcmp(argv[i], "--enable-psnr") == 0) {
            config.enable_psnr = true;
        } else if (strcmp(argv[i], "--min-psnr") == 0 && i + 1 < argc) {
            config.min_psnr = atof(argv[++i]);
        } else if (strcmp(argv[i], "--save-frames") == 0 && i + 1 < argc) {
            config.save_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--max-frames") == 0 && i + 1 < argc) {
            config.max_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            config.threads = atoi(argv[++i]);
            if (config.threads < 1) config.threads = 1;
        } else if (strcmp(argv[i], "--resolution") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &config.width, &config.height) != 2) {
                fprintf(stderr, "Invalid resolution format. Use WxH (e.g., 1920x1080)\n");
                exit(1);
            }
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            config.fps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--save-all") == 0) {
            // 兼容旧选项
            config.save_frames = -1;
        } else if (strcmp(argv[i], "--auto-test") == 0 || strcmp(argv[i], "--all-tests") == 0) {
            // 已经在循环前处理过，这里跳过
            continue;
        } else if (argv[i][0] == '-') {
            // 未知选项
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            exit(1);
        }
        // 注意：非选项参数（视频文件）已经在循环前处理
    }
    
    return config;
}

/**
 * @brief 自动测试用例结构体
 */
struct AutoTestCase {
    const char* name;           // 测试用例名称
    const char* description;    // 测试描述
    int width;                  // 视频宽度
    int height;                 // 视频高度
    int fps;                    // 目标帧率
    CodecType codec_type;       // 编解码器类型
    const char* profile;        // Profile (h264: high/main, h265: main, mjpeg: none)
    const char* video_file;     // 视频文件名（相对于当前目录）
};

// 自动测试用例数组（根据图片中的参数组合）
// 每种参数组合测试三种格式：mjpeg, h264, h265
static const AutoTestCase AUTO_TEST_CASES[] = {
    // 参数组合1: 1920×1080, 60fps, High
    {"T01", "1920×1080 @ 60fps - MJPEG", 1920, 1080, 60, CodecType::MJPEG, "none", "test_mjpeg_1920x1080_60fps_none.mp4"},
    {"T02", "1920×1080 @ 60fps - H264 High", 1920, 1080, 60, CodecType::H264, "high", "test_h264_1920x1080_60fps_high.mp4"},
    {"T03", "1920×1080 @ 60fps - H265 Main", 1920, 1080, 60, CodecType::H265, "main", "test_h265_1920x1080_60fps_main.mp4"},
    
    // 参数组合2: 2560×1440, 30fps, High
    {"T04", "2560×1440 @ 30fps - MJPEG", 2560, 1440, 30, CodecType::MJPEG, "none", "test_mjpeg_2560x1440_30fps_none.mp4"},
    {"T05", "2560×1440 @ 30fps - H264 High", 2560, 1440, 30, CodecType::H264, "high", "test_h264_2560x1440_30fps_high.mp4"},
    {"T06", "2560×1440 @ 30fps - H265 Main", 2560, 1440, 30, CodecType::H265, "main", "test_h265_2560x1440_30fps_main.mp4"},
    
    // 参数组合3: 3840×2160, 30fps, High
    {"T07", "3840×2160 @ 30fps - MJPEG", 3840, 2160, 30, CodecType::MJPEG, "none", "test_mjpeg_3840x2160_30fps_none.mp4"},
    {"T08", "3840×2160 @ 30fps - H264 High", 3840, 2160, 30, CodecType::H264, "high", "test_h264_3840x2160_30fps_high.mp4"},
    {"T09", "3840×2160 @ 30fps - H265 Main", 3840, 2160, 30, CodecType::H265, "main", "test_h265_3840x2160_30fps_main.mp4"},
};

static const int NUM_AUTO_TEST_CASES = sizeof(AUTO_TEST_CASES) / sizeof(AUTO_TEST_CASES[0]);

// 前向声明
int test_mp4_decode(const TestConfig& config);

/**
 * @brief 自动执行所有测试用例
 */
bool run_auto_tests(const TestConfig& base_config) {
    LOG_INFO("");
    LOG_INFO("╔═══════════════════════════════════════════════════════╗");
    LOG_INFO("║  Auto Test Suite - Running All Test Cases             ║");
    LOG_INFO("╚═══════════════════════════════════════════════════════╝");
    LOG_INFO("");
    LOG_INFO_FMT("Base configuration:");
    LOG_INFO_FMT("  Threads: %d", base_config.threads);
    LOG_INFO_FMT("  Display: %s", base_config.enable_display ? "enabled" : "disabled");
    LOG_INFO_FMT("  PSNR: %s", base_config.enable_psnr ? "enabled" : "disabled");
    if (base_config.enable_psnr) {
        LOG_INFO_FMT("  Min PSNR: %.2f dB", base_config.min_psnr);
    }
    LOG_INFO_FMT("  Save frames: %d", base_config.save_frames);
    LOG_INFO_FMT("  Max frames: %d", base_config.max_frames);
    LOG_INFO("");
    
    int total_tests = NUM_AUTO_TEST_CASES;
    int passed_tests = 0;
    int failed_tests = 0;
    int skipped_tests = 0;
    
    LOG_INFO("╔═══════════════════════════════════════════════════════╗");
    LOG_INFO("║  Running Test Cases                                    ║");
    LOG_INFO("╚═══════════════════════════════════════════════════════╝");
    LOG_INFO("");
    
    for (int i = 0; i < NUM_AUTO_TEST_CASES; i++) {
        const AutoTestCase& test_case = AUTO_TEST_CASES[i];
        LOG_INFO("─────────────────────────────────────────────────────────");
        LOG_INFO_FMT("Test Case %s: %s", test_case.name, test_case.description);
        LOG_INFO("─────────────────────────────────────────────────────────");
        
        // 检查视频文件是否存在
        if (access(test_case.video_file, F_OK) != 0) {
            skipped_tests++;
            LOG_WARN_FMT("⚠️  Test %s SKIPPED: Video file not found: %s", 
                        test_case.name, test_case.video_file);
            LOG_INFO("");
            continue;
        }
        
        // 创建测试配置
        TestConfig test_config = base_config;
        test_config.video_path = test_case.video_file;
        test_config.codec_type = test_case.codec_type;
        test_config.profile = test_case.profile;
        test_config.width = test_case.width;
        test_config.height = test_case.height;
        test_config.fps = test_case.fps;
        
        // 执行测试
        LOG_INFO_FMT("Video file: %s", test_case.video_file);
        LOG_INFO_FMT("Codec: %s, Profile: %s", 
                    test_config.getCodecName(), test_case.profile);
        LOG_INFO_FMT("Resolution: %dx%d @ %dfps", 
                    test_case.width, test_case.height, test_case.fps);
        LOG_INFO("");
        
        int result = test_mp4_decode(test_config);
        bool success = (result == 0);
        
        if (success) {
            passed_tests++;
            LOG_INFO_FMT("✅ Test %s PASSED", test_case.name);
        } else {
            failed_tests++;
            LOG_ERROR_FMT("❌ Test %s FAILED", test_case.name);
        }
        LOG_INFO("");
        
        // 短暂延迟，避免资源竞争
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    
    // 输出测试总结
    LOG_INFO("");
    LOG_INFO("╔═══════════════════════════════════════════════════════╗");
    LOG_INFO("║  Test Summary                                          ║");
    LOG_INFO("╚═══════════════════════════════════════════════════════╝");
    LOG_INFO_FMT("Total tests: %d", total_tests);
    LOG_INFO_FMT("Passed: %d ✅", passed_tests);
    LOG_INFO_FMT("Failed: %d ❌", failed_tests);
    if (skipped_tests > 0) {
        LOG_INFO_FMT("Skipped: %d ⏭️  (video file not found)", skipped_tests);
    }
    int executed_tests = total_tests - skipped_tests;
    LOG_INFO_FMT("Success rate: %.1f%% (%d/%d executed)", 
                executed_tests > 0 ? (passed_tests * 100.0 / executed_tests) : 0.0,
                passed_tests, executed_tests);
    LOG_INFO("");
    
    return failed_tests == 0;
}

/**
 * MP4视频解码测试主函数（支持H264/H265/MJPEG等多种编码格式）
 */
int test_mp4_decode(const TestConfig& config) {
    LOG_INFO("╔═══════════════════════════════════════════════════════╗");
    LOG_INFO("║  Video Decode Test                                    ║");
    LOG_INFO("╚═══════════════════════════════════════════════════════╝");
    LOG_INFO_FMT("Video file: %s", config.video_path);
    LOG_INFO_FMT("Codec: %s", config.getCodecName());
    if (config.codec_type == CodecType::H264) {
        LOG_INFO_FMT("Profile: %s", config.profile);
    }
    LOG_INFO_FMT("Decoder: %s", config.getDecoderName());
    LOG_INFO_FMT("Resolution: %dx%d @ %dfps", config.width, config.height, config.fps);
    LOG_INFO_FMT("Threads: %d", config.threads);
    LOG_INFO_FMT("Display output: %s", config.enable_display ? "enabled" : "disabled");
    if (config.save_frames == -1) {
        LOG_INFO("Save frames: all");
    } else if (config.save_frames == 0) {
        LOG_INFO("Save frames: none");
    } else {
        LOG_INFO_FMT("Save frames: first %d", config.save_frames);
    }
    if (config.max_frames == -1) {
        LOG_INFO("Max frames: unlimited");
    } else {
        LOG_INFO_FMT("Max frames: %d", config.max_frames);
    }
    if (config.output_path) {
        LOG_INFO_FMT("Output file: %s", config.output_path);
    }
    LOG_INFO("");
    
    // ========== 第1步：配置解码器 ==========
    LOG_INFO("[Step 1/8] Configuring decoder...");
    
    // 配置解码器输出格式 - 统一使用 ARGB（CPU 可访问）
    DecoderConfigBuilder decoderBuilder;
    decoderBuilder.setDecoderName(config.getDecoderName());
    
    if (strcmp(config.getDecoderName(), "h264_taco") == 0 || 
        strcmp(config.getDecoderName(), "hevc_taco") == 0) {
        // h264_taco/hevc_taco 输出 NV12（原生YUV格式，CPU 可访问）
        auto tacoConfig = TacoConfigBuilder()
            .setRgbConfig(false, "", "bt709")  // ch1_rgb=false -> 输出NV12
            .build();
        
        const char* codec = (strcmp(config.getDecoderName(), "h264_taco") == 0) ? "h264" : "hevc";
        decoderBuilder.useTaco(codec, tacoConfig);
        LOG_INFO("  Decoder output: NV12 (YUV format, CPU accessible)");
    }
    
    auto workerConfig = WorkerConfigBuilder()
        .setFileConfig(
            FileConfigBuilder()
                .setFilePath(config.video_path)
                .build()
        )
        .setDisplayConfig(
            DisplayConfigBuilder()
                .setDisplayResolution(config.width, config.height)
                .setBitsPerPixel(32)
                .build()
        )
        .setDecoderConfig(decoderBuilder.build())
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
    
    LOG_INFO_FMT("✅ Decoder configured: %s, %dx%d, hardware acceleration", 
                 config.getDecoderName(), config.width, config.height);
    
    // ========== 第2步：初始化显示设备（可选） ==========
    LOG_INFO("[Step 2/8] Initializing display device...");
    
    std::unique_ptr<LinuxFramebufferDevice> display;
    bool has_display = false;
    
    if (config.enable_display) {
        display = std::make_unique<LinuxFramebufferDevice>();
        has_display = display->initialize(0);
        if (has_display) {
            LOG_INFO_FMT("✅ Display initialized: %dx%d @ %d bpp",
                        display->getWidth(), display->getHeight(), display->getBitsPerPixel());
        } else {
            LOG_WARN("⚠️  Display not available, continuing without display");
        }
    } else {
        LOG_INFO("ℹ️  Display disabled by user");
    }
    
    // ========== 第3步：创建生产线 ==========
    LOG_INFO("[Step 3/8] Creating VideoProductionLine...");
    
    VideoProductionLine producer(
        false,              // loop = false（不循环，解码一次）
        config.threads,     // thread_count
        false               // enable_monitor = false
    );
    
    // 设置错误回调
    producer.setErrorCallback([](const std::string& error) {
        LOG_ERROR_FMT("Decode Error: %s", error.c_str());
        g_running = false;
    });
    
    LOG_INFO_FMT("✅ VideoProductionLine created (%d producer threads)", config.threads);
    
    // ========== 第4步：启动生产线 ==========
    LOG_INFO("[Step 4/8] Starting decode...");
    
    if (!producer.start(workerConfig)) {
        LOG_ERROR("❌ Failed to start VideoProductionLine");
        return -1;
    }
    
    LOG_INFO("✅ Decoding started");
    
    // ========== 第5步：获取 BufferPool ==========
    LOG_INFO("[Step 5/8] Getting BufferPool...");
    
    uint64_t pool_id = producer.getWorkingBufferPoolId();
    if (pool_id == 0) {
        LOG_ERROR("❌ No working BufferPool ID available");
        producer.stop();
        return -1;
    }
    
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(pool_id);
    auto pool_sptr = pool_weak.lock();
    if (!pool_sptr) {
        LOG_ERROR("❌ BufferPool not found or destroyed");
        producer.stop();
        return -1;
    }
    
    LOG_INFO_FMT("✅ BufferPool: '%s' (ID: %lu)", 
                pool_sptr->getName().c_str(), pool_id);
    pool_sptr->printStats();
    
    // ========== 第6步：创建 BufferWriter ==========
    LOG_INFO("[Step 6/8] Creating BufferWriter...");
    
    using productionline::io::BufferWriter;
    std::unique_ptr<BufferWriter> writer;
    char output_yuv[256];
    AVPixelFormat output_format = AV_PIX_FMT_NONE;
    int actual_width = config.width;
    int actual_height = config.height;
    std::string format_name = "NV12";
    const char* file_ext = "yuv";
    std::string ffplay_format = "nv12";
    
    if (config.save_frames != 0) {
        // 等待第一个Buffer以检测实际格式
        LOG_INFO("   Waiting for first buffer to detect format...");
        Buffer* first_buffer = pool_sptr->acquireFilled(true, 5000);  // 5秒超时
        if (!first_buffer) {
            LOG_ERROR("❌ Failed to get first buffer (timeout)");
            producer.stop();
            return -1;
        }
        
        // 从Buffer元数据获取实际格式
        if (first_buffer->hasImageMetadata()) {
            output_format = first_buffer->getImageFormat();
            actual_width = first_buffer->getImageWidth();
            actual_height = first_buffer->getImageHeight();
            const char* fmt_name = av_get_pix_fmt_name(output_format);
            format_name = fmt_name ? fmt_name : "NV12";
            
            // 设置ffplay格式（NV12对应nv12）
            if (output_format == AV_PIX_FMT_NV12) {
                ffplay_format = "nv12";
            } else if (output_format == AV_PIX_FMT_YUV420P) {
                ffplay_format = "yuv420p";
            } else {
                ffplay_format = format_name;
            }
            
            LOG_INFO_FMT("   Detected format: %s (%dx%d)", format_name.c_str(), actual_width, actual_height);
        } else {
            // 默认使用NV12
            output_format = AV_PIX_FMT_NV12;
            format_name = "NV12";
            ffplay_format = "nv12";
            LOG_WARN("   Buffer has no metadata, using default NV12");
        }
        
        // 创建BufferWriter
        writer = std::make_unique<BufferWriter>();
        if (config.output_path) {
            snprintf(output_yuv, sizeof(output_yuv), "%s", config.output_path);
        } else {
            snprintf(output_yuv, sizeof(output_yuv), 
                    "/tmp/decoded_%dx%d_%ld.%s", actual_width, actual_height, time(nullptr), file_ext);
        }
        
        if (!writer->open(output_yuv, output_format, actual_width, actual_height)) {
            LOG_ERROR_FMT("❌ Failed to open BufferWriter: %s", output_yuv);
            pool_sptr->releaseFilled(first_buffer);
            producer.stop();
            return -1;
        }
        
        LOG_INFO_FMT("✅ BufferWriter opened: %s (format: %s, %dx%d)", 
                    output_yuv, format_name.c_str(), actual_width, actual_height);
        
        // 保存第一帧
        if (writer->write(first_buffer)) {
            LOG_INFO("   ✅ Saved first frame");
        }
        pool_sptr->releaseFilled(first_buffer);
    } else {
        LOG_INFO("ℹ️  Output file disabled (save_frames = 0)");
    }
    
    // ========== 第7步：消费者循环（解码+显示+保存） ==========
    LOG_INFO("[Step 7/8] Consuming decoded frames...");
    LOG_INFO("Press Ctrl+C to stop early");
    LOG_INFO("");
    
    int frame_count = 0;
    int display_count = 0;
    int save_count = 0;
    int save_limit = (config.save_frames == -1) ? INT32_MAX : config.save_frames;
    int max_frames_limit = (config.max_frames == -1) ? INT32_MAX : config.max_frames;
    
    auto start_time = std::chrono::steady_clock::now();
    auto last_report_time = start_time;
    
    while (g_running && frame_count < max_frames_limit) {
        // 从 BufferPool 获取已解码的 Buffer
        Buffer* buffer = pool_sptr->acquireFilled(true, 100);  // 超时100ms
        
        if (!buffer) {
            // 超时：检查生产者是否还在运行
            if (!producer.isRunning()) {
                LOG_INFO("Producer stopped, exiting consumer loop");
                break;
            }
            continue;  // 超时但生产者还在，继续等待
        }
        
        // 显示到屏幕（如果启用）
        if (has_display) {
            display->waitVerticalSync();
            if (display->displayBufferByDMA(buffer)) {
                display_count++;
            } else {
                // DMA 失败，回退到普通显示
                display->displayFilledFramebuffer(buffer);
                display_count++;
            }
        }
        
        // 保存到文件（使用BufferWriter）
        if (writer && save_count < save_limit) {
            if (writer->write(buffer)) {
                save_count++;
            }
        }
        
        // 归还 Buffer
        pool_sptr->releaseFilled(buffer);
        frame_count++;
        
        // 每 60 帧（约1秒）打印一次进度
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_report_time).count();
        
        if (elapsed >= 1000) {  // 每秒报告一次
            double current_fps = producer.getAverageFPS();
            LOG_INFO_FMT("Progress: %d frames | FPS: %.1f | Display: %d | Saved: %d",
                        frame_count, current_fps, display_count, save_count);
            last_report_time = now;
        }
    }
    
    // 排空剩余的 Buffer
    LOG_INFO("Draining remaining buffers...");
    Buffer* remaining = nullptr;
    int drained = 0;
    while ((remaining = pool_sptr->acquireFilled(false, 0)) != nullptr) {
        if (has_display) {
            display->waitVerticalSync();
            display->displayBufferByDMA(remaining);
            display_count++;
        }
        if (writer && save_count < save_limit) {
            if (writer->write(remaining)) {
                save_count++;
            }
        }
        pool_sptr->releaseFilled(remaining);
        frame_count++;
        drained++;
    }
    if (drained > 0) {
        LOG_INFO_FMT("Drained %d remaining buffers", drained);
    }
    
    if (writer) {
        writer->close();
        LOG_INFO_FMT("✅ BufferWriter closed: %d frames written", writer->getWriteCount());
    }
    
    auto end_time = std::chrono::steady_clock::now();
    auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();
    
    LOG_INFO("✅ Consuming completed");
    
    // ========== 第8步：停止生产线并输出统计 ==========
    LOG_INFO("[Step 8/8] Stopping and generating report...");
    
    producer.stop();
    
    // 计算性能指标
    double decode_fps = producer.getAverageFPS();
    double realtime_fps = (frame_count * 1000.0) / total_duration;
    double target_fps = config.fps;
    bool fps_meets_requirement = decode_fps >= target_fps;
    if (config.enable_psnr) {
        fps_meets_requirement = true;  // PSNR模式下不检查FPS
    }
    
    // ========== 输出测试报告 ==========
    LOG_INFO("");
    LOG_INFO("╔═══════════════════════════════════════════════════════╗");
    LOG_INFO("║  Test Report: Video Decode                            ║");
    LOG_INFO("╚═══════════════════════════════════════════════════════╝");
    LOG_INFO_FMT("Video file: %s", config.video_path);
    LOG_INFO_FMT("Codec: %s", config.getCodecName());
    LOG_INFO_FMT("Decoder: %s (hardware)", config.getDecoderName());
    LOG_INFO_FMT("Resolution: %dx%d", config.width, config.height);
    LOG_INFO_FMT("Target FPS: %.0f", target_fps);
    LOG_INFO("");
    LOG_INFO("--- Performance Metrics ---");
    LOG_INFO_FMT("Total frames decoded: %d", frame_count);
    LOG_INFO_FMT("Frames displayed: %d", display_count);
    LOG_INFO_FMT("Frames saved: %d", save_count);
    LOG_INFO_FMT("Total time: %.2f seconds", total_duration / 1000.0);
    LOG_INFO_FMT("Decode FPS (producer): %.2f", decode_fps);
    LOG_INFO_FMT("Realtime FPS (overall): %.2f", realtime_fps);
    LOG_INFO_FMT("Frames produced: %d", producer.getProducedFrames());
    LOG_INFO_FMT("Frames skipped: %d", producer.getSkippedFrames());
    LOG_INFO("");
    LOG_INFO("--- Result ---");
    if (config.enable_psnr) {
        LOG_INFO("ℹ️  FPS check skipped (PSNR mode prioritizes accuracy over speed)");
    } else if (fps_meets_requirement) {
        LOG_INFO_FMT("✅ PASS: Decode FPS (%.2f) >= Target FPS (%.0f)", 
                    decode_fps, target_fps);
    } else {
        LOG_WARN_FMT("⚠️  WARN: Decode FPS (%.2f) < Target FPS (%.0f)",
                    decode_fps, target_fps);
    }
    
    if (writer && save_count > 0) {
        LOG_INFO_FMT("📁 Saved data: %s", output_yuv);
        LOG_INFO_FMT("   Format: %s, Resolution: %dx%d, Frames: %d", 
                    format_name.c_str(), actual_width, actual_height, save_count);
        LOG_INFO("   You can verify with FFmpeg:");
        LOG_INFO_FMT("   ffplay -f rawvideo -pix_fmt %s -s %dx%d %s", 
                    ffplay_format.c_str(), actual_width, actual_height, output_yuv);
    }
    
    LOG_INFO("");
    LOG_INFO("--- BufferPool Final Stats ---");
    pool_sptr->printStats();
    LOG_INFO("╚═══════════════════════════════════════════════════════╝");
    
    // ========== PSNR 验证（如果启用） ==========
    bool psnr_pass = true;
    if (config.enable_psnr && save_count > 0) {
        psnr_pass = validate_psnr_streaming(
            output_yuv,
            config.video_path,
            config.width,
            config.height,
            save_count,
            config.min_psnr
        );
    } else if (config.enable_psnr && save_count == 0) {
        LOG_WARN("⚠️  PSNR validation requested but no frames saved (use --save-frames)");
        psnr_pass = false;
    }
    
    // 最终判断：
    // - 如果启用PSNR：只看PSNR结果
    // - 如果未启用PSNR：看FPS是否达标
    bool final_result;
    if (config.enable_psnr) {
        final_result = psnr_pass;  // PSNR模式下只看质量，不看FPS
    } else {
        final_result = fps_meets_requirement;  // 性能模式下看FPS
    }
    
    return final_result ? 0 : -1;
}

/**
 * 主函数
 */
int main(int argc, char* argv[]) {
    // 初始化日志系统
    INIT_LOGGER();
    
    // 注册信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 解析命令行参数
    TestConfig config = parse_arguments(argc, argv);
    
    // 运行测试
    int result = 0;
    
    if (config.auto_test) {
        // 自动测试模式：执行所有测试用例
        bool success = run_auto_tests(config);
        result = success ? 0 : -1;
    } else {
        // 普通模式：执行单个测试
        // 检查视频文件是否存在
        if (!config.video_path) {
            LOG_ERROR("❌ Video file not specified");
            return 1;
        }
        
        if (access(config.video_path, F_OK) != 0) {
            LOG_ERROR_FMT("❌ Video file not found: %s", config.video_path);
            return 1;
        }
        
        LOG_INFO_FMT("✅ Video file exists: %s", config.video_path);
        
        // 运行测试
        result = test_mp4_decode(config);
    }
    
    // 输出最终结果
    LOG_INFO("");
    if (result == 0) {
        LOG_INFO("╔═══════════════════════════════════════════════════════╗");
        LOG_INFO("║  ✅ TEST PASSED                                        ║");
        LOG_INFO("╚═══════════════════════════════════════════════════════╝");
    } else {
        LOG_INFO("╔═══════════════════════════════════════════════════════╗");
        LOG_INFO("║  ❌ TEST FAILED                                        ║");
        LOG_INFO("╚═══════════════════════════════════════════════════════╝");
    }
    
    return result;
}

