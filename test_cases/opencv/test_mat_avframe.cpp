// test_mat_avframe.cpp
// 测试 cv::Mat(AVFrame*) 接口：解码 input.mp4 前10帧并转换为 Mat

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#include <opencv2/core.hpp>
#include <opencv2/core/tacv.hpp>
#include <opencv2/opencv.hpp>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <iostream>
#include <cstring>

// 将 OpenCV type 转换为字符串
std::string typeToString(int type) {
    // 从 type 提取通道数和深度
    int depth = type & CV_MAT_DEPTH_MASK;
    int chans = 1 + (type >> CV_CN_SHIFT);

    std::string depthStr;
    switch (depth) {
        case CV_8U:  depthStr = "CV_8U"; break;
        case CV_8S:  depthStr = "CV_8S"; break;
        case CV_16U: depthStr = "CV_16U"; break;
        case CV_16S: depthStr = "CV_16S"; break;
        case CV_32S: depthStr = "CV_32S"; break;
        case CV_32F: depthStr = "CV_32F"; break;
        case CV_64F: depthStr = "CV_64F"; break;
        default:     depthStr = "Unknown"; break;
    }

    return depthStr + "C" + std::to_string(chans);
}

// 根据 Mat 的 rows/cols/height 推断是否是 NV12 格式
std::string detectFormat(const cv::Mat& mat, int frame_height) {
    // NV12 完整格式：rows = height * 3 / 2
    // 只有 Y 平面：rows = height
    if (mat.rows == frame_height) {
        return "YUV_NV12_Y_ONLY";  // 只有 Y 平面
    } else if (mat.rows == frame_height * 3 / 2) {
        return "YUV_NV12_FULL";    // 完整 NV12 (Y + UV)
    } else if (mat.rows == frame_height * 2) {
        return "YUV_NV12_YV12";    // YV12 或其他
    }
    return "UNKNOWN";
}

int main(int argc, char* argv[]) {
    const char* filename = "/usr/data/vdec/input.mp4";
    int out_w = 320;
    int out_h = 320;
    bool save_images = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            filename = argv[++i];
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            out_w = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            out_h = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0) {
            save_images = true;
        }
    }

    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, filename, nullptr, nullptr) < 0) {
        fprintf(stderr, "Cannot open %s\n", filename);
        return 1;
    }
    avformat_find_stream_info(fmt_ctx, nullptr);

    // 找视频流
    int video_idx = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_idx = (int)i;
            break;
        }
    }
    if (video_idx < 0) { fprintf(stderr, "No video stream\n"); return 1; }

    AVCodecParameters* par = fmt_ctx->streams[video_idx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder_by_name("h264_taco");
    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, par);
    avcodec_open2(codec_ctx, codec, nullptr);

    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frame = av_frame_alloc();
    int frame_count = 0;

    while (frame_count < 10 && av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index != video_idx) { av_packet_unref(pkt); continue; }

        if (avcodec_send_packet(codec_ctx, pkt) == 0) {
            while (avcodec_receive_frame(codec_ctx, frame) == 0 && frame_count < 10) {
                // ---- 核心接口测试 ----
                // cv::Mat(frame) 是零拷贝，Mat 引用 AVFrame 数据
                // 用花括号缩小作用域，让 Mat 在 av_frame_unref 之前析构
                {
                    cv::Mat test_mat = cv::Mat(frame);
                    printf("Before Frame %2d: Mat %dx%d type=%s format=%s\n",
                           frame_count, test_mat.cols, test_mat.rows,
                           typeToString(test_mat.type()).c_str(),
                           detectFormat(test_mat, frame->height).c_str());
                    frame_count++;

                    // 保存 resize 前的图片（需要先转为 BGR）
                    if (save_images) {
                        cv::Mat bgr_mat;
                        cv::cvtColor(test_mat, bgr_mat, cv::COLOR_YUV2BGR_NV12);
                        char filename[256];
                        snprintf(filename, sizeof(filename), "before_%02d.jpg", frame_count);
                        cv::imwrite(filename, bgr_mat);
                        printf("Saved %s\n", filename);
                    }

                    // 动态获取 test_mat 的格式：按 frame->height 比例推算输出行数
                    // 例如 NV12 完整格式 rows = height*3/2，则 out_rows = out_h*3/2
                    // int out_rows = out_h * test_mat.rows / frame->height;
                    // cv::Mat output_mat(out_rows, out_w, test_mat.type());
                    cv::Mat output_mat;
                    output_mat.allocator = cv::hal::getAllocator();
                    cv::resize(test_mat, output_mat, cv::Size(out_w, out_h), 0, 0, 1);

                    // 只取 Y 平面，使 output_mat 和 test_mat 布局一致 (Y-only: rows = out_h)
                    output_mat = output_mat.rowRange(0, out_h);

                    printf("After Frame %2d: Mat %dx%d type=%s format=%s\n",
                           frame_count, output_mat.cols, output_mat.rows,
                           typeToString(output_mat.type()).c_str(),
                           detectFormat(output_mat, out_h).c_str());

                    // 保存 resize 后的图片（需要先转为 BGR）
                    if (save_images) {
                        cv::Mat bgr_mat;
                        cv::cvtColor(output_mat, bgr_mat, cv::COLOR_YUV2BGR_NV12);
                        char filename[256];
                        snprintf(filename, sizeof(filename), "after_%02d.jpg", frame_count);
                        cv::imwrite(filename, bgr_mat);
                        printf("Saved %s\n", filename);
                    }
                }  // <-- test_mat 在这里析构
                // ----------------------
                av_frame_unref(frame);
            }
        }
        av_packet_unref(pkt);
    }

    printf("Decoded %d frames\n", frame_count);

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    return 0;
}
