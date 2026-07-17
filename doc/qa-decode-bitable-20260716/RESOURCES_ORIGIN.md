# QA 测试资源来源与本次新增文件

## 1. 板端 `/usr/data/qa/` 资源从哪来（两类）

| 类别 | 内容 | 来源 | 如何同步 |
|------|------|------|----------|
| **OwnCloud 资源** | 主要为 `.yuv` / `.rgb`（约 206 个数据文件）+ `README.txt`，仅有 `128x128.jpg` | OwnCloud 内部分享 | `sync_qa_resources.sh` |
| **tps-test 包资源** | `.mp4` / `.jpg` / `.ini`（约 42 个） | apt 包 `tps-test` | `apt-get install -y tps-test` |

### OwnCloud 下载地址（脚本内置）

- Host: `http://owncloud.intchains.in:9000`
- 分享 token: `Z6zVDaVpjNrI80s`
- 密码: `123456`
- 脚本：`taco-component-bitable-test/scripts/sync_qa_resources.sh`
- 板端执行：`cd /usr/data/qa && sh /root/sync_qa_resources.sh`

当前 OwnCloud 列表统计（2026-07-17 拉取）：**208 项** = 120×rgb + 86×yuv + 1×txt + 1×jpg（`128x128.jpg`）。  
**不含** mp4 / avi / 本次新增的多数 jpg。

### tps-test 包内已有 jpg/mp4（节选）

含：`1920x1080.jpg`、`2000x1125.jpg`、`2560x1440.jpg`、`4096x2560.jpg`、`7680x4320.jpg`、`*_h264_*fps.mp4`、`*_h265_*fps.mp4`、`48x48_yuvj420p.jpg` 等。  
**不含**：`48x48.jpg`、`320x240.jpg`、`640x480.jpg`、`1280x720.jpg`、`3840x2160.jpg`、以及任何 `*_60fps.avi/.mjpeg`。

### README 说明的 raw 生成源

板端 `/usr/data/qa/README.txt`：

- 源视频：`/home/jojo/Downloads/proxy_source.mp4`（历史生成记录）
- 多数 yuv/rgb 由该源按用例占位符解析生成（每文件约 10 帧）

---

## 2. 本次新生成资源统计（Procedure 实际需要）

本机目录：`/tmp/qa_missing_assets/`  
板端目录：`192.168.57.113:/usr/data/qa/`  
打包副本：`doc/qa-decode-bitable-20260716/new_assets/` + `new_qa_assets_20260717.tar.gz`（约 40MB）

| 文件 | 大小 | 说明 | Procedure 使用 |
|------|------|------|----------------|
| `48x48.jpg` | 1.7K | 由板端 `1920x1080.jpg` 缩放 | 是 |
| `320x240.jpg` | 36K | 同上 | 是 |
| `640x480.jpg` | 120K | 同上 | 是 |
| `1280x720.jpg` | 299K | 同上 | 是 |
| `3840x2160.jpg` | 1.2M | 同上 | 是 |
| `48x48_60fps.avi` | 214K | 由 `640x480_h264_60fps.mp4` 缩放转 MJPEG，180帧/60fps/3s | 是 |
| `640x480_60fps.avi` | 6.2M | 由 `640x480_h264_60fps.mp4` 转 MJPEG | 是 |
| `1920x1080_60fps.avi` | 14M | 由 `1920x1080_h264_60fps.mp4` 转 MJPEG | 是 |
| `*_60fps.mjpeg` | 另存 | 同源，但裸流时间基为 25fps，**用例已改用 avi** | 否（备份） |

---

## 3. 核心问题是什么

1. **缺口文件只落在当前板 + 本机临时目录**，既不在 OwnCloud，也不在 `tps-test` deb。  
2. **重刷系统 / 换板 / 只跑 sync_qa_resources.sh** → 这 8 个 jpg/avi **会再次缺失**。  
3. OwnCloud 公共分享 **只读**（WebDAV PUT 返回 403），脚本账号无法直接上传；需有写权限的同事在网页端上传，或把文件打进 `tps-test` 包。

### 入库结论（已调研路径形态）

| 仓库 | 存放形态 | 典型文件 |
|------|----------|----------|
| **OwnCloud** | **单个文件平铺**（无 zip/tar）| `.yuv` / `.rgb` + `README.txt`（仅 1 个 jpg） |
| **`tps-test` deb** | **单个文件**装到 `/usr/data/qa/`（无资源压缩包）| `.mp4` / `.jpg` / `.ini` |

本次新增是 **jpg + avi（MJPEG 码流）**，与 **`tps-test` 同类**，**不是** OwnCloud 的 raw yuv/rgb。

**应并入 `tps-test`，以单个文件形式打包进 deb**（不要上传一个大压缩包到 OwnCloud；OwnCloud 分享也是散文件且当前只读 403）。

建议打进包的文件（散装）：
- `48x48.jpg` `320x240.jpg` `640x480.jpg` `1280x720.jpg` `3840x2160.jpg`
- `48x48_60fps.avi` `640x480_60fps.avi` `1920x1080_60fps.avi`

本地已备好散文件与 tar（仅便于拷贝，入库仍按单个文件进 deb）：
`doc/qa-decode-bitable-20260716/new_assets/`  
`doc/qa-decode-bitable-20260716/new_qa_assets_20260717.tar.gz`
