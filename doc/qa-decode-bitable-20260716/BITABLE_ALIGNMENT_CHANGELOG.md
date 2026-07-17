# QA_DECODE 多维表对齐变更记录（2026-07-17）

依据：用例名（Test case）为准；Procedure 分辨率与之对齐；超 H.264/H.265 规格期望 FAILED；错位拆出分辨率新建用例。

硬件规格：[解码模块](https://intchains.feishu.cn/wiki/FwsPwRdPniJdiZk3wKncjkChnBc)
- H.264/H.265 最大 4096×2160
- JPEG 最大 32768×32768
- PP 不支持上缩放

## 1. Procedure 对齐用例名（17 条）

| ID | 用例名 | Procedure 现对齐分辨率 |
|----|--------|----------------------|
| TC-1537 | mjpeg解码320x240 | 320x240.jpg |
| TC-1538 | mjpeg解码640x480 | 640x480.jpg |
| TC-1539 | mjpeg解码60fps640x480 | 640x480_60fps.mjpeg |
| TC-1540 | mjpeg解码1280x720 | 1280x720.jpg |
| TC-1541 | mjpeg解码1920x1080 | 1920x1080.jpg |
| TC-1542 | mjpeg解码60fps1920x1080 | 1920x1080_60fps.mjpeg |
| TC-1543 | mjpeg解码2560x1440 | 2560x1440.jpg |
| TC-1544 | mjpeg解码3840x2160 | 3840x2160.jpg |
| TC-1545 | mjpeg质量验证解码128x128 | 128x128.jpg + PSNR/SSIM |
| TC-1546 | mjpeg质量验证解码320x240 | 320x240.jpg + PSNR/SSIM |
| TC-1547 | mjpeg质量验证解码640x480 | 640x480.jpg + PSNR/SSIM |
| TC-1548 | mjpeg质量验证解码60fps640x480 | 640x480_60fps.mjpeg + PSNR/SSIM |
| TC-1549 | mjpeg质量验证解码1280x720 | 1280x720.jpg + PSNR/SSIM |
| TC-1550 | mjpeg质量验证解码1920x1080 | 1920x1080.jpg + PSNR/SSIM |
| TC-1551 | mjpeg质量验证解码60fps1920x1080 | 1920x1080_60fps.mjpeg + PSNR/SSIM |
| TC-1552 | mjpeg质量验证解码2560x1440 | 2560x1440.jpg + PSNR/SSIM |
| TC-1553 | mjpeg质量验证解码3840x2160 | 3840x2160.jpg + PSNR/SSIM |

复查：MJPEG 用例名 vs `--resolution` 不匹配数 = **0**。

## 2. Expectation 改为 FAILED（4 条，超 H.264 规格）

| ID | 用例名 | Expectation |
|----|--------|-------------|
| TC-1560 | multi_pp_crop2 | FAILED；超出 H.264/H.265 硬解上限 4096×2160 |
| TC-1562 | multi_pp_crop4 | FAILED；超出 H.264/H.265 硬解上限 4096×2160 |
| TC-1572 | multi_pp_scale1 | FAILED；超出 H.264/H.265 硬解上限 4096×2160 |
| TC-1574 | multi_pp_scale3 | FAILED；超出 H.264/H.265 硬解上限 4096×2160 |

## 3. 新建用例（12 条，原 TC-NEW-01～12）

| 自动编号 | 用例名 | 可执行 |
|----------|--------|--------|
| TC-3200 | mjpeg解码48x48 | 是 |
| TC-3201 | mjpeg解码60fps48x48 | 否 |
| TC-3202 | mjpeg质量验证解码48x48 | 是 |
| TC-3203 | mjpeg质量验证解码60fps48x48 | 否 |
| TC-3204 | mjpeg解码2000x1125 | 是 |
| TC-3205 | mjpeg质量验证解码2000x1125 | 是 |
| TC-3206 | mjpeg解码1920x1367 | 是 |
| TC-3207 | mjpeg质量验证解码1920x1367 | 是 |
| TC-3208 | mjpeg解码4096x2560 | 是 |
| TC-3209 | mjpeg解码7680x4320 | 是 |
| TC-3210 | mjpeg质量验证解码4096x2560 | 是 |
| TC-3211 | mjpeg质量验证解码7680x4320 | 是 |

说明：60fps 用例 Procedure 使用 `{res}_60fps.mjpeg`，素材未就绪前保持「测试例可执行=否」。

## 4. 未改 Expectation 的大图 JPEG

JPEG 上限内（如 7680×4320、32768×32768）仍为 PASSED，符合硬件规格。
