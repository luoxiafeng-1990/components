// test_mat_avframe.cpp
// 测试 cv::Mat(AVFrame*) 接口：解码 input.mp4 前10帧并转换为 Mat

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/core/tacv.hpp>
#include <cstdio>
#include <string>
#include <iostream>

int main(int argc, char* argv[]) {
    const char* filename = (argc > 1) ? argv[1] : "/usr/data/vdec/input.mp4";

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
    const AVCodec* codec = avcodec_find_decoder_by_name("h264");
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
                    printf("Frame %2d: Mat %dx%d type=%d data=%p\n",
                           frame_count, test_mat.cols, test_mat.rows, test_mat.type(), test_mat.data);
                    frame_count++;

                    std::string output_file = "output" + std::to_string(frame_count) + ".jpg";
                    cv::imwrite(output_file, test_mat);
                    std::cout << "Mat used, now exiting scope..." << std::endl;
                }  // <-- test_mat 在这里析构
                // ----------------------

                std::cout << "Mat destructed, now av_frame_unref..." << std::endl;
                av_frame_unref(frame);
                std::cout << "av_frame_unref done" << std::endl;
            }
        }
        std::cout << "hello,world2" << std::endl;
        av_packet_unref(pkt);
        std::cout << "hello,world2" << std::endl;
    }

    printf("Decoded %d frames\n", frame_count);

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    return 0;
}
