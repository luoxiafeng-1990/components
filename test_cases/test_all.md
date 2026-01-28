# QA Cases 测试命令手册

本文档包含所有 `qa_cases` 测试命令，按模块分类，可逐条执行验证。

## 环境说明

- **远程主机**: root@192.168.56.48
- **测试程序**: ~/qa_cases
- **测试文件目录**: /usr/data/ffmpeg/
- **密码**: 123456
- **测试项总计**: 171 项

---

## 1. 部署与帮助

```bash
# 部署程序到远程主机
sshpass -p '123456' scp -o StrictHostKeyChecking=no \
    /home/ubuntu/intchains/workshop-debian/output/products/taco_mes20/ea65xx/ubuntu_server/target/usr/local/bin/qa_cases \
    root@192.168.56.48:~/

# 全局帮助
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 "~/qa_cases -h"

# VDEC 模块帮助
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 "~/qa_cases vdec -h"

# PP 模块帮助
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 "~/qa_cases pp -h"

# Record 模块帮助
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 "~/qa_cases record -h"

# Writer 模块帮助
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 "~/qa_cases writer -h"

# 列出所有预定义测试
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 "~/qa_cases vdec -l"
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 "~/qa_cases pp -l"
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 "~/qa_cases record -l"
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 "~/qa_cases writer -l"
```

---

## 2. VDEC 视频解码测试

### 2.1 H.264 解码测试

```bash
# H.264 128x128
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 128x128 --psnr --ssim --display /usr/data/ffmpeg/128x128.mp4"

# H.264 320x240
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 320x240 --psnr --ssim --display /usr/data/ffmpeg/320x240.mp4"

# H.264 640x480
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 640x480 --psnr --ssim --display /usr/data/ffmpeg/640x480.mp4"

# H.264 640x480 60fps
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 640x480 --fps 60 --psnr --ssim --display /usr/data/ffmpeg/640x480_60fps.mp4"

# H.264 720p (1280x720)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 1280x720 --psnr --ssim --display /usr/data/ffmpeg/1280x720.mp4"

# H.264 1080p (1920x1080)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 1920x1080 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# H.264 1080p 60fps
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 1920x1080 --fps 60 --psnr --ssim --display /usr/data/ffmpeg/1920x1080_60fps.mp4"

# H.264 2560x1440
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 2560x1440 --psnr --ssim --display /usr/data/ffmpeg/H264_2560_1440_24fps_10s.mp4"

# H.264 4K (3840x2160)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 3840x2160 --psnr --ssim --display /usr/data/ffmpeg/3840x2160.mp4"
```

### 2.2 H.265/HEVC 解码测试

```bash
# H.265 128x128
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h265 --resolution 128x128 --psnr --ssim --display /usr/data/ffmpeg/128x128_hevc.mp4"

# H.265 320x240
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h265 --resolution 320x240 --psnr --ssim --display /usr/data/ffmpeg/320x240_hevc.mp4"

# H.265 640x480
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h265 --resolution 640x480 --psnr --ssim --display /usr/data/ffmpeg/640x480_hevc.mp4"

# H.265 640x480 60fps
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h265 --resolution 640x480 --fps 60 --psnr --ssim --display /usr/data/ffmpeg/640x480_hevc_60fps.mp4"

# H.265 720p (1280x720)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h265 --resolution 1280x720 --psnr --ssim --display /usr/data/ffmpeg/1280x720_hevc.mp4"

# H.265 1080p (1920x1080)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h265 --resolution 1920x1080 --psnr --ssim --display /usr/data/ffmpeg/1920x1080_hevc.mp4"

# H.265 1080p 60fps
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h265 --resolution 1920x1080 --fps 60 --psnr --ssim --display /usr/data/ffmpeg/1920x1080_hevc_60fps.mp4"

# H.265 2560x1440
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h265 --resolution 2560x1440 --psnr --ssim --display /usr/data/ffmpeg/H265_2560_1440_24fps_12s.mp4"

# H.265 4K (3840x2160)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h265 --resolution 3840x2160 --psnr --ssim --display /usr/data/ffmpeg/3840x2160_hevc.mp4"
```

### 2.3 MJPEG 解码测试

```bash
# MJPEG 128x128
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec mjpeg --resolution 128x128 --psnr --ssim --display /usr/data/ffmpeg/128x128.jpg"

# MJPEG 320x240
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec mjpeg --resolution 320x240 --psnr --ssim --display /usr/data/ffmpeg/320x240.jpg"

# MJPEG 640x480
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec mjpeg --resolution 640x480 --psnr --ssim --display /usr/data/ffmpeg/640x480.jpg"

# MJPEG 640x480 60fps
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec mjpeg --resolution 640x480 --fps 60 --psnr --ssim --display /usr/data/ffmpeg/640x480_60fps.mjpeg"

# MJPEG 720p (1280x720)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec mjpeg --resolution 1280x720 --psnr --ssim --display /usr/data/ffmpeg/1280x720.jpg"

# MJPEG 1080p (1920x1080)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec mjpeg --resolution 1920x1080 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.jpg"

# MJPEG 1080p 60fps
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec mjpeg --resolution 1920x1080 --fps 60 --psnr --ssim --display /usr/data/ffmpeg/1920x1080_60fps.mjpeg"

# MJPEG 2560x1440
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec mjpeg --resolution 2560x1440 --psnr --ssim --display /usr/data/ffmpeg/2560x1440.jpg"

# MJPEG 4K (3840x2160)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec mjpeg --resolution 3840x2160 --psnr --ssim --display /usr/data/ffmpeg/3840x2160.jpg"
```

### 2.4 软件解码测试

```bash
# 软件解码 H.264 1080p
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec software --resolution 1920x1080 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# 软件解码 H.265 1080p
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec software --resolution 1920x1080 --psnr --ssim --display /usr/data/ffmpeg/1920x1080_hevc.mp4"
```

### 2.5 RTSP 流解码测试

```bash
# RTSP H.264 720p
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 1280x720 --psnr --ssim --display --rtsp rtsp://192.168.1.100/stream"

# RTSP H.264 1080p
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 1920x1080 --psnr --ssim --display --rtsp rtsp://192.168.1.100/stream"

# RTSP H.264 4K
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 3840x2160 --psnr --ssim --display --rtsp rtsp://192.168.1.100/stream"

# RTSP H.265 720p
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h265 --resolution 1280x720 --psnr --ssim --display --rtsp rtsp://192.168.1.100/stream"

# RTSP H.265 1080p
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h265 --resolution 1920x1080 --psnr --ssim --display --rtsp rtsp://192.168.1.100/stream"

# RTSP H.265 4K
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h265 --resolution 3840x2160 --psnr --ssim --display --rtsp rtsp://192.168.1.100/stream"
```

### 2.6 多 Worker / 多线程测试

```bash
# 多 Worker 测试 - HW+SW 同时解码 1080p
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec multi_worker --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# 多 Worker 测试 - HW+SW 同时解码 4K
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec multi_worker_4k --psnr --ssim --display /usr/data/ffmpeg/3840x2160.mp4"

# 2 线程解码
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec multithread_2 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# 4 线程解码
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec multithread_4 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# 8 线程解码
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec multithread_8 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"
```

### 2.7 带参数的解码测试

```bash
# 限制最大帧数 (只解码前100帧)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 1920x1080 --max-frames 100 /usr/data/ffmpeg/1920x1080.mp4"

# 保存解码帧到文件 (保存前10帧)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 1920x1080 --save 10 /usr/data/ffmpeg/1920x1080.mp4"

# 保存到指定路径
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 1920x1080 --save 10 --output /tmp/decode_output.yuv /usr/data/ffmpeg/1920x1080.mp4"

# 启用显示输出 (输出到 framebuffer)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 1920x1080 --display /usr/data/ffmpeg/1920x1080.mp4"

# 显示 + 限制帧数
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 1920x1080 --display --max-frames 300 /usr/data/ffmpeg/1920x1080.mp4"

# 详细日志模式
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 1920x1080 --verbose /usr/data/ffmpeg/1920x1080.mp4"
```

### 2.8 PSNR/SSIM 质量验证测试

```bash
# H.264 720p PSNR 验证
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 1280x720 --psnr /usr/data/ffmpeg/1280x720.mp4"

# H.264 1080p PSNR 验证
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 1920x1080 --psnr /usr/data/ffmpeg/1920x1080.mp4"

# H.264 4K PSNR 验证
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 3840x2160 --psnr /usr/data/ffmpeg/3840x2160.mp4"

# H.264 PSNR 验证（自定义阈值 35dB）
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 1920x1080 --psnr --min-psnr 35 /usr/data/ffmpeg/1920x1080.mp4"

# H.265 720p PSNR 验证
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h265 --resolution 1280x720 --psnr /usr/data/ffmpeg/1280x720_hevc.mp4"

# H.265 1080p PSNR 验证
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h265 --resolution 1920x1080 --psnr /usr/data/ffmpeg/1920x1080_hevc.mp4"

# H.265 4K PSNR 验证
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h265 --resolution 3840x2160 --psnr /usr/data/ffmpeg/3840x2160_hevc.mp4"

# H.264 1080p SSIM 验证
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 1920x1080 --ssim /usr/data/ffmpeg/1920x1080.mp4"

# H.265 1080p SSIM 验证
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h265 --resolution 1920x1080 --ssim /usr/data/ffmpeg/1920x1080_hevc.mp4"

# SSIM 验证（自定义阈值 0.98）
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 1920x1080 --ssim --min-ssim 0.98 /usr/data/ffmpeg/1920x1080.mp4"

# H.264 1080p PSNR + SSIM 联合验证
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 1920x1080 --psnr --ssim /usr/data/ffmpeg/1920x1080.mp4"

# H.265 1080p PSNR + SSIM 联合验证
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h265 --resolution 1920x1080 --psnr --ssim /usr/data/ffmpeg/1920x1080_hevc.mp4"

# 4K PSNR + SSIM 联合验证
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 3840x2160 --psnr --ssim /usr/data/ffmpeg/3840x2160.mp4"

# 自定义双阈值验证（PSNR >= 38dB 且 SSIM >= 0.97）
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 1920x1080 --psnr --min-psnr 38 --ssim --min-ssim 0.97 /usr/data/ffmpeg/1920x1080.mp4"

# 只验证前 100 帧
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 1920x1080 --psnr --ssim --max-frames 100 /usr/data/ffmpeg/1920x1080.mp4"

# 只验证前 300 帧 + 详细日志
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 1920x1080 --psnr --ssim --max-frames 300 --verbose /usr/data/ffmpeg/1920x1080.mp4"

# 使用外部参考文件进行 PSNR 对比（--reference）
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --resolution 1920x1080 --psnr --reference /tmp/reference.yuv /usr/data/ffmpeg/1920x1080.mp4"

# 单独指定宽高（--width 和 --height）
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases vdec --codec h264 --width 1920 --height 1080 /usr/data/ffmpeg/1920x1080.mp4"
```

---

## 3. PP 后处理格式测试

### 3.1 PP0 YUV 格式测试

```bash
# PP0 NV12
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp0_nv12 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP0 NV21
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp0_nv21 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP0 I420
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp0_i420 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP0 YV12
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp0_yv12 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP0 P010 (10-bit)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp0_p010 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP0 NV16
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp0_nv16 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP0 NV61
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp0_nv61 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP0 I422
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp0_i422 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP0 NV24
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp0_nv24 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP0 I444
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp0_i444 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"
```

### 3.2 PP1 RGB 格式测试

```bash
# PP1 ARGB888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_argb888 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP1 ABGR888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_abgr888 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP1 RGBA888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_rgba888 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP1 BGRA888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_bgra888 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP1 RGB888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_rgb888 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP1 BGR888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_bgr888 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP1 XRGB888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_xrgb888 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP1 XBGR888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_xbgr888 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP1 RGBX888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_rgbx888 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP1 BGRX888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_bgrx888 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP1 RGB888 Planar
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_rgb888_planar --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP1 BGR888 Planar
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_bgr888_planar --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP1 R16G16B16 (16-bit)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_r16g16b16 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP1 B16G16R16 (16-bit)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_b16g16r16 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP1 GBRP
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_gbrp --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"
```

### 3.3 PP1 YUV 格式测试

```bash
# PP1 NV12
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_nv12 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP1 NV21
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_nv21 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP1 I420
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_i420 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP1 YV12
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_yv12 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP1 P010 (10-bit)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_p010 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP1 NV16
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_nv16 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP1 NV61
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_nv61 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP1 I422
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_i422 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP1 NV24
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_nv24 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# PP1 I444
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_i444 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"
```

### 3.4 Multi-PP 测试

```bash
# Multi-PP T01: NV12 + RGB888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp multi_pp_t01 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# Multi-PP T02: NV12 + ARGB888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp multi_pp_t02 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# Multi-PP T03: NV12 + BGR888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp multi_pp_t03 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# Multi-PP T04: NV12 + RGBA888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp multi_pp_t04 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# Multi-PP T05: I420 + RGB888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp multi_pp_t05 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# Multi-PP T06: I420 + ARGB888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp multi_pp_t06 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# Multi-PP T07: NV21 + RGB888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp multi_pp_t07 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# Multi-PP T09: P010 + RGB888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp multi_pp_t09 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# Multi-PP T10: NV12 + RGB888 (BT709)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp multi_pp_t10 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# Multi-PP T11: NV12 + ARGB888 (BT2020)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp multi_pp_t11 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"
```

### 3.5 Crop/Scale 测试

```bash
# Crop1: 左上角 (0,0) 裁剪 960x540
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp multi_pp_crop1 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# Crop2: 右下角 (960,540) 裁剪 960x540
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp multi_pp_crop2 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# Crop3: 中心 (480,270) 裁剪 960x540
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp multi_pp_crop3 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# Crop4: 上半部分 (0,0) 裁剪 1920x540
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp multi_pp_crop4 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# Crop5: 左半部分 (0,0) 裁剪 960x1080
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp multi_pp_crop5 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"

# Crop6: 小区域 (100,100) 裁剪 320x240
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp multi_pp_crop6 --psnr --ssim --display /usr/data/ffmpeg/1920x1080.mp4"
```

### 3.6 PP 带参数测试

```bash
# PP 显示输出
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_argb888 --display /usr/data/ffmpeg/1920x1080.mp4"

# PP 限制帧数
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_argb888 --max-frames 100 /usr/data/ffmpeg/1920x1080.mp4"

# PP 保存帧到文件
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_argb888 --save 10 --output /tmp/pp_output.rgb /usr/data/ffmpeg/1920x1080.mp4"

# PP 显示 + 限制帧数
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_argb888 --display --max-frames 300 /usr/data/ffmpeg/1920x1080.mp4"

# PP 详细日志模式（--verbose）
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp pp1_argb888 --verbose /usr/data/ffmpeg/1920x1080.mp4"

# PP 指定颜色标准 BT709（--color-std）
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp --channel pp1 --format argb888 --color-std bt709 /usr/data/ffmpeg/1920x1080.mp4"

# PP 指定颜色标准 BT2020
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp --channel pp1 --format argb888 --color-std bt2020 /usr/data/ffmpeg/1920x1080.mp4"

# PP 单独指定宽高（--width 和 --height）
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp --channel pp1 --format argb888 --width 1920 --height 1080 /usr/data/ffmpeg/1920x1080.mp4"

# PP 手动指定裁剪区域（--crop x,y,w,h）
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp --channel pp1 --format argb888 --crop 100,100,800,600 /usr/data/ffmpeg/1920x1080.mp4"

# PP 手动指定输入文件（--input）
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases pp --input /usr/data/ffmpeg/1920x1080.mp4 --channel pp1 --format argb888"
```

---

## 4. Record 录制测试

### 4.1 RTSP 录制测试

```bash
# RTSP 录制为 MP4 (10秒)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases record --rtsp rtsp://192.168.1.100/stream --format mp4 --duration 10 --output /tmp/record_test.mp4"

# RTSP 录制为 MKV (10秒)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases record --rtsp rtsp://192.168.1.100/stream --format mkv --duration 10 --output /tmp/record_test.mkv"

# RTSP 录制为 MOV (10秒)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases record --rtsp rtsp://192.168.1.100/stream --format mov --duration 10 --output /tmp/record_test.mov"

# RTSP 录制为 TS (10秒)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases record --rtsp rtsp://192.168.1.100/stream --format ts --duration 10 --output /tmp/record_test.ts"

# RTSP 录制为 FLV (10秒)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases record --rtsp rtsp://192.168.1.100/stream --format flv --duration 10 --output /tmp/record_test.flv"

# RTSP 录制为 AVI (10秒)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases record --rtsp rtsp://192.168.1.100/stream --format avi --duration 10 --output /tmp/record_test.avi"

# RTSP 录制为 3GP (10秒)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases record --rtsp rtsp://192.168.1.100/stream --format 3gp --duration 10 --output /tmp/record_test.3gp"
```

### 4.2 长时间录制测试

```bash
# RTSP 长时间录制 MP4 (60秒)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases record --rtsp rtsp://192.168.1.100/stream --format mp4 --duration 60 --output /tmp/record_long.mp4"

# RTSP 长时间录制 MKV (60秒)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases record --rtsp rtsp://192.168.1.100/stream --format mkv --duration 60 --output /tmp/record_long.mkv"
```

### 4.3 文件重封装测试

```bash
# 文件重封装为 MP4
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases record --file /usr/data/ffmpeg/1920x1080.mp4 --format mp4 --output /tmp/remux_test.mp4"

# 文件重封装为 MKV
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases record --file /usr/data/ffmpeg/1920x1080.mp4 --format mkv --output /tmp/remux_test.mkv"

# 文件重封装为 TS
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases record --file /usr/data/ffmpeg/1920x1080.mp4 --format ts --output /tmp/remux_test.ts"
```

### 4.4 Record 带参数测试

```bash
# 测试所有输出格式（--all-formats）
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases record --rtsp rtsp://192.168.1.100/stream --all-formats --duration 5 --output /tmp/record_all"

# 录制详细日志模式（--verbose）
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases record --rtsp rtsp://192.168.1.100/stream --format mp4 --duration 10 --verbose --output /tmp/record_verbose.mp4"

# 使用 --input 替代 --file
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases record --input /usr/data/ffmpeg/1920x1080.mp4 --format mkv --output /tmp/remux_input.mkv"
```

---

## 5. Writer BufferWriter 格式测试

### 5.1 RGB 格式输出测试

```bash
# RGB ARGB8888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer rgb_argb888 /usr/data/ffmpeg/1920x1080.mp4"

# RGB ABGR8888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer rgb_abgr888 /usr/data/ffmpeg/1920x1080.mp4"

# RGB RGBA8888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer rgb_rgba888 /usr/data/ffmpeg/1920x1080.mp4"

# RGB BGRA8888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer rgb_bgra888 /usr/data/ffmpeg/1920x1080.mp4"

# RGB RGB888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer rgb_rgb888 /usr/data/ffmpeg/1920x1080.mp4"

# RGB BGR888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer rgb_bgr888 /usr/data/ffmpeg/1920x1080.mp4"

# RGB XRGB8888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer rgb_xrgb888 /usr/data/ffmpeg/1920x1080.mp4"

# RGB XBGR8888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer rgb_xbgr888 /usr/data/ffmpeg/1920x1080.mp4"

# RGB RGBX8888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer rgb_rgbx888 /usr/data/ffmpeg/1920x1080.mp4"

# RGB BGRX8888
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer rgb_bgrx888 /usr/data/ffmpeg/1920x1080.mp4"

# RGB R16G16B16 (16-bit)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer rgb_r16g16b16 /usr/data/ffmpeg/1920x1080.mp4"

# RGB B16G16R16 (16-bit)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer rgb_b16g16r16 /usr/data/ffmpeg/1920x1080.mp4"
```

### 5.2 YUV 格式输出测试

```bash
# YUV NV12
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer yuv_nv12 /usr/data/ffmpeg/1920x1080.mp4"

# YUV NV21
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer yuv_nv21 /usr/data/ffmpeg/1920x1080.mp4"

# YUV I420
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer yuv_i420 /usr/data/ffmpeg/1920x1080.mp4"

# YUV YV12
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer yuv_yv12 /usr/data/ffmpeg/1920x1080.mp4"

# YUV P010 (10-bit)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer yuv_p010 /usr/data/ffmpeg/1920x1080.mp4"

# YUV NV16
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer yuv_nv16 /usr/data/ffmpeg/1920x1080.mp4"

# YUV NV61
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer yuv_nv61 /usr/data/ffmpeg/1920x1080.mp4"

# YUV I422
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer yuv_i422 /usr/data/ffmpeg/1920x1080.mp4"

# YUV NV24
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer yuv_nv24 /usr/data/ffmpeg/1920x1080.mp4"

# YUV I444
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer yuv_i444 /usr/data/ffmpeg/1920x1080.mp4"
```

### 5.3 批量格式测试

```bash
# 批量测试所有 RGB 格式 (12个)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer all_rgb /usr/data/ffmpeg/1920x1080.mp4"

# 批量测试所有 YUV 格式 (10个)
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer all_yuv /usr/data/ffmpeg/1920x1080.mp4"
```

### 5.4 Writer 带参数测试

```bash
# 使用 --all-rgb 选项批量测试所有 RGB 格式
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer --input /usr/data/ffmpeg/1920x1080.mp4 --all-rgb"

# 使用 --all-yuv 选项批量测试所有 YUV 格式
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer --input /usr/data/ffmpeg/1920x1080.mp4 --all-yuv"

# 指定输出路径（--output）
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer rgb_argb888 --output /tmp/writer_output.rgb /usr/data/ffmpeg/1920x1080.mp4"

# 指定保存帧数（--frames）
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer rgb_argb888 --frames 10 /usr/data/ffmpeg/1920x1080.mp4"

# 指定输出格式（--format）
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer --input /usr/data/ffmpeg/1920x1080.mp4 --format nv12 --output /tmp/writer_nv12.yuv"

# 详细日志模式（--verbose）
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer rgb_argb888 --verbose /usr/data/ffmpeg/1920x1080.mp4"

# 组合：指定格式 + 帧数 + 输出路径
sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
    "~/qa_cases writer --input /usr/data/ffmpeg/1920x1080.mp4 --format argb888 --frames 20 --output /tmp/output.rgb --verbose"
```

---

## 6. 批量测试脚本

```bash
#!/bin/bash
# 批量执行所有 VDEC 测试
for codec in h264 h265; do
    for res in 128x128 320x240 640x480 1280x720 1920x1080 3840x2160; do
        echo "Testing $codec $res..."
        sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
            "~/qa_cases vdec --codec $codec --resolution $res /usr/data/ffmpeg/${res}.mp4"
    done
done

# 批量执行所有 PP0 测试
for fmt in nv12 nv21 i420 yv12 p010 nv16 nv61 i422 nv24 i444; do
    echo "Testing pp0_$fmt..."
    sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
        "~/qa_cases pp pp0_$fmt /usr/data/ffmpeg/1920x1080.mp4"
done

# 批量执行所有 PP1 RGB 测试
for fmt in argb888 abgr888 rgba888 bgra888 rgb888 bgr888 xrgb888 xbgr888 rgbx888 bgrx888; do
    echo "Testing pp1_$fmt..."
    sshpass -p '123456' ssh -o StrictHostKeyChecking=no root@192.168.56.48 \
        "~/qa_cases pp pp1_$fmt /usr/data/ffmpeg/1920x1080.mp4"
done
```

---

## 附录 A. 参数说明

### A.1 VDEC 模块参数

| 参数 | 说明 |
|------|------|
| `-h, --help` | 显示帮助信息 |
| `-l, --list` | 列出所有预定义测试 |
| `-f, --file <path>` | 输入视频文件路径 |
| `-r, --rtsp <url>` | RTSP URL |
| `-c, --codec <name>` | 编解码器 (h264/h265/mjpeg/software) |
| `-W, --width <n>` | 视频宽度 |
| `-H, --height <n>` | 视频高度 |
| `-R, --resolution <WxH>` | 分辨率 (等同于 -W 和 -H 组合) |
| `-F, --fps <n>` | 目标帧率 |
| `-m, --max-frames <n>` | 最大帧数 (-1=无限制) |
| `-s, --save <n>` | 保存帧数 (0=不保存, -1=全部) |
| `-o, --output <path>` | 输出文件路径 |
| `-d, --display` | 启用显示输出 |
| `-p, --psnr` | 启用 PSNR 验证 |
| `-S, --ssim` | 启用 SSIM 验证 |
| `-P, --min-psnr <n>` | PSNR 阈值 (默认: 30.0 dB) |
| `-M, --min-ssim <n>` | SSIM 阈值 (默认: 0.95) |
| `-e, --reference <path>` | 外部参考文件路径 (用于 PSNR/SSIM 对比) |
| `-v, --verbose` | 详细日志 |

### A.2 PP 模块参数

| 参数 | 说明 |
|------|------|
| `-h, --help` | 显示帮助信息 |
| `-l, --list` | 列出所有预定义测试 |
| `-i, --input <path>` | 输入视频路径 |
| `-f, --format <fmt>` | 输出格式 (nv12/argb888/...) |
| `-c, --channel <ch>` | 通道选择 (pp0/pp1/multi) |
| `-W, --width <n>` | 输出宽度 |
| `-H, --height <n>` | 输出高度 |
| `-R, --resolution <WxH>` | 分辨率 (等同于 -W 和 -H 组合) |
| `-C, --crop <x,y,w,h>` | 裁剪区域 |
| `-s, --color-std <s>` | 颜色标准 (bt601/bt709/bt2020) |
| `-o, --output <path>` | 输出文件路径 |
| `-n, --save <n>` | 保存帧数 |
| `-d, --display` | 启用显示输出 |
| `-m, --max-frames <n>` | 最大帧数 |
| `-v, --verbose` | 详细日志 |

### A.3 Record 模块参数

| 参数 | 说明 |
|------|------|
| `-h, --help` | 显示帮助信息 |
| `-l, --list` | 列出所有预定义测试 |
| `-r, --rtsp <url>` | RTSP 流 URL |
| `-f, --file <path>` | 输入文件路径 |
| `-i, --input <path>` | 输入路径 (等同于 -f) |
| `-o, --output <path>` | 输出文件路径 |
| `-F, --format <fmt>` | 输出格式 (mp4/mkv/ts/flv/avi/mov/3gp) |
| `-d, --duration <sec>` | 录制时长（秒） |
| `-a, --all-formats` | 测试所有输出格式 |
| `-v, --verbose` | 详细日志 |

### A.4 Writer 模块参数

| 参数 | 说明 |
|------|------|
| `-h, --help` | 显示帮助信息 |
| `-l, --list` | 列出所有预定义测试 |
| `-i, --input <path>` | 输入视频路径 |
| `-f, --format <fmt>` | 输出格式 |
| `-o, --output <path>` | 输出文件路径 |
| `-n, --frames <n>` | 保存帧数 |
| `-R, --all-rgb` | 测试所有 RGB 格式 (12个) |
| `-Y, --all-yuv` | 测试所有 YUV 格式 (10个) |
| `-v, --verbose` | 详细日志 |

---

## 附录 B. 测试统计模板

| 模块 | 测试项 | 平均 FPS | PSNR (dB) | SSIM | 状态 |
|------|--------|----------|-----------|------|------|
| VDEC | h264_1920x1080 | | | | [ ] PASS |
| VDEC | h265_1920x1080 | | | | [ ] PASS |
| VDEC | psnr_h264_1080p | | | | [ ] PASS |
| PP | pp0_nv12 | | | | [ ] PASS |
| PP | pp1_argb888 | | | | [ ] PASS |
| Record | rtsp_to_mp4 | | | | [ ] PASS |
| Writer | rgb_argb888 | | | | [ ] PASS |

---

## 附录 C. 测试方法论

### C.1 测试执行流程

```
┌─────────────────────────────────────────────────────────────────┐
│                        测试执行流程                              │
├─────────────────────────────────────────────────────────────────┤
│  1. 遍历文档中所有测试命令                                        │
│  2. 逐条执行测试命令，捕获完整日志                                │
│  3. 分析日志，验证命令逻辑是否正确执行                            │
│  4. 验证输出文件（视频/图像）是否符合预期                         │
│  5. 记录测试结果到报告                                           │
│  6. 支持失败用例复测                                             │
└─────────────────────────────────────────────────────────────────┘
```

### C.2 日志分析要点

#### C.2.1 必须检查的日志关键字

```bash
# 错误关键字（出现即失败）
ERROR|FATAL|FAIL|Segmentation fault|core dumped|Aborted|Exception

# 警告关键字（需要关注）
WARN|WARNING|timeout|retry

# 成功关键字（必须出现）
PASS|SUCCESS|completed|finished
```

#### C.2.2 日志验证规则

| 命令参数 | 日志中必须出现 | 说明 |
|----------|---------------|------|
| `--codec h264` | `codec: h264` 或 `h264_taco` | 验证编解码器选择正确 |
| `--resolution 1920x1080` | `width: 1920, height: 1080` | 验证分辨率设置正确 |
| `--max-frames 100` | `max_frames: 100` 且帧数≤100 | 验证帧数限制生效 |
| `--display` | `display: enabled` 或 `framebuffer` | 验证显示输出启用 |
| `--psnr` | `PSNR` 计算结果 | 验证 PSNR 验证启用 |
| `--ssim` | `SSIM` 计算结果 | 验证 SSIM 验证启用 |
| `--save 10` | `saved 10 frames` 或输出文件存在 | 验证帧保存功能 |
| `--output /tmp/xxx` | 文件 `/tmp/xxx` 存在 | 验证输出路径正确 |
| `--format argb888` | `format: ARGB888` | 验证输出格式正确 |
| `--color-std bt709` | `color_standard: BT709` | 验证颜色标准设置 |
| `--crop x,y,w,h` | `crop: x,y,w,h` | 验证裁剪区域设置 |

### C.3 输出文件验证方法

#### C.3.1 视频文件验证

```bash
# 使用 ffprobe 检查视频属性
ffprobe -v error -show_entries stream=width,height,codec_name,pix_fmt -of csv=p=0 <output_file>

# 验证视频时长
ffprobe -v error -show_entries format=duration -of csv=p=0 <output_file>

# 验证视频帧数
ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 <output_file>
```

#### C.3.2 原始帧文件验证

```bash
# 验证 YUV 文件大小（NV12 格式）
# 预期大小 = width * height * 1.5 * frame_count
expected_size=$((1920 * 1080 * 3 / 2 * 10))  # 10帧 NV12
actual_size=$(stat -c%s output.yuv)
[ "$actual_size" -eq "$expected_size" ] && echo "PASS" || echo "FAIL"

# 验证 RGB 文件大小（ARGB8888 格式）
# 预期大小 = width * height * 4 * frame_count
expected_size=$((1920 * 1080 * 4 * 10))  # 10帧 ARGB8888
actual_size=$(stat -c%s output.rgb)
[ "$actual_size" -eq "$expected_size" ] && echo "PASS" || echo "FAIL"
```

#### C.3.3 各格式预期文件大小公式

| 格式 | 每帧字节数 | 公式 |
|------|-----------|------|
| NV12/NV21 | W × H × 1.5 | `width * height * 3 / 2` |
| I420/YV12 | W × H × 1.5 | `width * height * 3 / 2` |
| P010 | W × H × 3 | `width * height * 3` |
| NV16/NV61 | W × H × 2 | `width * height * 2` |
| I422 | W × H × 2 | `width * height * 2` |
| NV24 | W × H × 3 | `width * height * 3` |
| I444 | W × H × 3 | `width * height * 3` |
| RGB888/BGR888 | W × H × 3 | `width * height * 3` |
| ARGB8888/RGBA8888 等 | W × H × 4 | `width * height * 4` |
| R16G16B16 | W × H × 6 | `width * height * 6` |

### C.4 自动化测试脚本

#### C.4.1 完整测试执行脚本

```bash
#!/bin/bash
# test_runner.sh - QA Cases 自动化测试脚本

# 配置
REMOTE_HOST="root@192.168.56.48"
REMOTE_PASS="123456"
QA_CASES="~/qa_cases"
TEST_DATA="/usr/data/ffmpeg"
REPORT_FILE="test_report_$(date +%Y%m%d_%H%M%S).md"
LOG_DIR="test_logs"

# 创建日志目录
mkdir -p "$LOG_DIR"

# SSH 命令封装
ssh_cmd() {
    sshpass -p "$REMOTE_PASS" ssh -o StrictHostKeyChecking=no "$REMOTE_HOST" "$1"
}

# 初始化测试报告
init_report() {
    cat > "$REPORT_FILE" << 'EOF'
# QA Cases 测试报告

**测试时间**: $(date '+%Y-%m-%d %H:%M:%S')
**测试主机**: root@192.168.56.48

## 测试结果汇总

<table border="1" cellpadding="8" cellspacing="0" style="border-collapse: collapse; width: 100%;">
<thead>
<tr style="background-color: #f0f0f0;">
<th>序号</th>
<th>模块</th>
<th>测试项</th>
<th>命令</th>
<th>日志验证</th>
<th>输出验证</th>
<th>状态</th>
<th>备注</th>
</tr>
</thead>
<tbody>
EOF
}

# 添加测试结果到报告
add_result() {
    local idx="$1"
    local module="$2"
    local test_name="$3"
    local command="$4"
    local log_check="$5"
    local output_check="$6"
    local status="$7"
    local notes="$8"
    
    local status_color
    if [ "$status" = "PASS" ]; then
        status_color="style=\"background-color: #90EE90;\""
    elif [ "$status" = "FAIL" ]; then
        status_color="style=\"background-color: #FFB6C1;\""
    else
        status_color="style=\"background-color: #FFFFE0;\""
    fi
    
    cat >> "$REPORT_FILE" << EOF
<tr>
<td>$idx</td>
<td>$module</td>
<td>$test_name</td>
<td><code>$command</code></td>
<td>$log_check</td>
<td>$output_check</td>
<td $status_color><b>$status</b></td>
<td>$notes</td>
</tr>
EOF
}

# 结束测试报告
finish_report() {
    local total="$1"
    local passed="$2"
    local failed="$3"
    
    cat >> "$REPORT_FILE" << EOF
</tbody>
</table>

## 统计信息

| 项目 | 数量 |
|------|------|
| 总测试数 | $total |
| 通过 | $passed |
| 失败 | $failed |
| 通过率 | $(echo "scale=2; $passed * 100 / $total" | bc)% |

---

**报告生成时间**: $(date '+%Y-%m-%d %H:%M:%S')
EOF
}

# 分析日志
analyze_log() {
    local log_file="$1"
    local command="$2"
    local result="PASS"
    local notes=""
    
    # 检查错误关键字
    if grep -qiE "ERROR|FATAL|FAIL|Segmentation|core dumped|Aborted|Exception" "$log_file"; then
        result="FAIL"
        notes="发现错误日志"
    fi
    
    # 根据命令参数验证日志内容
    if echo "$command" | grep -q "\-\-max-frames"; then
        max_frames=$(echo "$command" | grep -oP '\-\-max-frames\s+\K\d+')
        if ! grep -q "max_frames.*$max_frames\|frames.*$max_frames" "$log_file"; then
            result="WARN"
            notes="$notes; max-frames 参数未在日志中确认"
        fi
    fi
    
    if echo "$command" | grep -q "\-\-psnr"; then
        if ! grep -qi "PSNR" "$log_file"; then
            result="FAIL"
            notes="$notes; PSNR 计算未执行"
        fi
    fi
    
    if echo "$command" | grep -q "\-\-ssim"; then
        if ! grep -qi "SSIM" "$log_file"; then
            result="FAIL"
            notes="$notes; SSIM 计算未执行"
        fi
    fi
    
    if echo "$command" | grep -q "\-\-display"; then
        if ! grep -qi "display\|framebuffer" "$log_file"; then
            result="WARN"
            notes="$notes; display 启用未在日志中确认"
        fi
    fi
    
    echo "$result|$notes"
}

# 验证输出文件
verify_output() {
    local command="$1"
    local result="PASS"
    local notes=""
    
    # 提取输出路径
    local output_path=$(echo "$command" | grep -oP '\-\-output\s+\K\S+')
    
    if [ -n "$output_path" ]; then
        # 检查文件是否存在
        if ! ssh_cmd "[ -f $output_path ]"; then
            result="FAIL"
            notes="输出文件不存在: $output_path"
        else
            # 检查文件大小
            local file_size=$(ssh_cmd "stat -c%s $output_path 2>/dev/null || echo 0")
            if [ "$file_size" -eq 0 ]; then
                result="FAIL"
                notes="输出文件为空: $output_path"
            else
                # 如果是视频文件，使用 ffprobe 验证
                if echo "$output_path" | grep -qE '\.(mp4|mkv|ts|avi|mov|flv)$'; then
                    local video_info=$(ssh_cmd "ffprobe -v error -show_entries stream=width,height -of csv=p=0 $output_path 2>/dev/null")
                    if [ -z "$video_info" ]; then
                        result="FAIL"
                        notes="视频文件无效: $output_path"
                    else
                        notes="视频信息: $video_info"
                    fi
                fi
            fi
        fi
    fi
    
    echo "$result|$notes"
}

# 执行单个测试
run_test() {
    local idx="$1"
    local module="$2"
    local test_name="$3"
    local command="$4"
    
    local log_file="$LOG_DIR/test_${idx}_${module}_${test_name}.log"
    
    echo "[$idx] 执行测试: $module - $test_name"
    
    # 执行命令并捕获日志
    ssh_cmd "$command" > "$log_file" 2>&1
    local exit_code=$?
    
    # 分析日志
    local log_result=$(analyze_log "$log_file" "$command")
    local log_check=$(echo "$log_result" | cut -d'|' -f1)
    local log_notes=$(echo "$log_result" | cut -d'|' -f2-)
    
    # 验证输出
    local output_result=$(verify_output "$command")
    local output_check=$(echo "$output_result" | cut -d'|' -f1)
    local output_notes=$(echo "$output_result" | cut -d'|' -f2-)
    
    # 综合判断
    local final_status="PASS"
    local final_notes=""
    
    if [ "$log_check" = "FAIL" ] || [ "$output_check" = "FAIL" ] || [ $exit_code -ne 0 ]; then
        final_status="FAIL"
    elif [ "$log_check" = "WARN" ] || [ "$output_check" = "WARN" ]; then
        final_status="WARN"
    fi
    
    [ -n "$log_notes" ] && final_notes="$log_notes"
    [ -n "$output_notes" ] && final_notes="$final_notes $output_notes"
    
    # 添加到报告
    add_result "$idx" "$module" "$test_name" "$command" "$log_check" "$output_check" "$final_status" "$final_notes"
    
    echo "$final_status"
}

# 主函数
main() {
    echo "====== QA Cases 自动化测试 ======"
    echo "开始时间: $(date)"
    
    init_report
    
    local total=0
    local passed=0
    local failed=0
    
    # 在此添加测试用例
    # run_test <序号> <模块> <测试名> <完整命令>
    
    # 示例：VDEC 测试
    result=$(run_test 1 "VDEC" "h264_1080p" "$QA_CASES vdec --codec h264 --resolution 1920x1080 $TEST_DATA/1920x1080.mp4")
    total=$((total + 1))
    [ "$result" = "PASS" ] && passed=$((passed + 1)) || failed=$((failed + 1))
    
    # ... 更多测试用例 ...
    
    finish_report $total $passed $failed
    
    echo "====== 测试完成 ======"
    echo "总计: $total, 通过: $passed, 失败: $failed"
    echo "报告文件: $REPORT_FILE"
}

main "$@"
```

#### C.4.2 复测脚本

```bash
#!/bin/bash
# retest_failed.sh - 复测失败用例脚本

REPORT_FILE="$1"
NEW_REPORT="retest_report_$(date +%Y%m%d_%H%M%S).md"

if [ -z "$REPORT_FILE" ]; then
    echo "用法: $0 <测试报告文件>"
    exit 1
fi

# 从报告中提取失败的测试用例
echo "从报告中提取失败用例..."
failed_cases=$(grep -E '<td.*FAIL.*</td>' "$REPORT_FILE" | grep -oP '<code>\K[^<]+')

if [ -z "$failed_cases" ]; then
    echo "没有发现失败的测试用例"
    exit 0
fi

echo "发现失败用例:"
echo "$failed_cases" | nl

# 复测
echo ""
echo "开始复测..."

# 初始化新报告（复用上面的函数）
# ... 执行复测逻辑 ...

echo "复测完成，报告: $NEW_REPORT"
```

### C.5 测试报告格式

#### C.5.1 报告模板

```markdown
# QA Cases 测试报告

**测试时间**: 2026-01-27 10:30:00
**测试主机**: root@192.168.56.48
**测试版本**: qa_cases v2.22

## 测试结果汇总

<table border="1" cellpadding="8" cellspacing="0" style="border-collapse: collapse; width: 100%;">
<thead>
<tr style="background-color: #f0f0f0;">
<th>序号</th>
<th>模块</th>
<th>测试项</th>
<th>命令</th>
<th>日志验证</th>
<th>输出验证</th>
<th>状态</th>
<th>备注</th>
</tr>
</thead>
<tbody>
<tr>
<td>1</td>
<td>VDEC</td>
<td>h264_1080p</td>
<td><code>qa_cases vdec --codec h264 --resolution 1920x1080 video.mp4</code></td>
<td>PASS</td>
<td>PASS</td>
<td style="background-color: #90EE90;"><b>PASS</b></td>
<td>FPS: 120.5</td>
</tr>
<tr>
<td>2</td>
<td>VDEC</td>
<td>psnr_h264_1080p</td>
<td><code>qa_cases vdec --codec h264 --psnr video.mp4</code></td>
<td>PASS</td>
<td>PASS</td>
<td style="background-color: #90EE90;"><b>PASS</b></td>
<td>PSNR: 42.5 dB</td>
</tr>
<tr>
<td>3</td>
<td>PP</td>
<td>pp1_argb888</td>
<td><code>qa_cases pp pp1_argb888 video.mp4</code></td>
<td>FAIL</td>
<td>N/A</td>
<td style="background-color: #FFB6C1;"><b>FAIL</b></td>
<td>ERROR: format not supported</td>
</tr>
</tbody>
</table>

## 统计信息

| 项目 | 数量 |
|------|------|
| 总测试数 | 171 |
| 通过 | 168 |
| 失败 | 3 |
| 通过率 | 98.25% |

## 失败用例详情

### 1. PP - pp1_argb888

- **命令**: `qa_cases pp pp1_argb888 video.mp4`
- **错误日志**:
```
[ERROR] Format ARGB888 not supported on this hardware
[ERROR] PP1 initialization failed
```
- **分析**: 硬件不支持 ARGB888 格式
- **建议**: 检查硬件配置或使用其他格式

---

**报告生成时间**: 2026-01-27 11:45:00
```

### C.6 测试检查清单

#### C.6.1 执行前检查

- [ ] 远程主机可访问 (`ping 192.168.56.48`)
- [ ] qa_cases 已部署到远程主机
- [ ] 测试视频文件存在 (`/usr/data/ffmpeg/`)
- [ ] 磁盘空间充足 (输出文件需要空间)
- [ ] 日志目录已创建

#### C.6.2 每条命令检查

- [ ] 命令执行无崩溃 (exit code = 0)
- [ ] 日志无 ERROR/FATAL 关键字
- [ ] 命令参数在日志中有体现
- [ ] 输出文件存在且非空
- [ ] 输出文件格式/大小符合预期
- [ ] PSNR/SSIM 值在合理范围内 (如适用)

#### C.6.3 执行后检查

- [ ] 所有测试用例已执行
- [ ] 测试报告已生成
- [ ] 失败用例已记录详细信息
- [ ] 日志文件已保存备查

---

**文档生成日期**: 2026-01-27
