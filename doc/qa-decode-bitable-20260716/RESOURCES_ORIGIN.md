# QA 测试资源来源与本次新增文件

## 0. 判定原则（以 Procedure 路径为准）

**不要凭文件后缀猜仓库**，看用例 Procedure 写的是哪个目录：

| Procedure 路径前缀 | 板端目录 | 来源 |
|--------------------|----------|------|
| `/usr/data/ffmpeg/...` | `/usr/data/ffmpeg/` | **`tps-test` apt 包** |
| `/usr/data/qa/...`（yuv/rgb 等 raw） | `/usr/data/qa/` | **OwnCloud**（`sync_qa_resources.sh`） |
| `/usr/data/qa/...`（mp4/jpg/ini 等） | `/usr/data/qa/` | 多数也来自 **`tps-test`**（与 ffmpeg 目录同包） |

板端 192.168.57.113 实测（`dpkg -L tps-test`）：

- `/usr/data/ffmpeg/`：**55** 个文件（全属 tps-test）
- `/usr/data/qa/`：tps-test 贡献 **42** 个（mp4/jpg/ini）；其余大量 yuv/rgb + README 来自 OwnCloud / 手工放入

因此：

- 「OwnCloud → `/usr/data/qa`」成立  
- 「tps-test → `/usr/data/ffmpeg`」成立  
- 补充：tps-test **同时**往 `/usr/data/qa` 装了解码用 mp4/jpg（Procedure 里大量 `vdec ... /usr/data/qa/*_h264_*.mp4` 即此类）

### Procedure 抽样

| Test set | 典型 Procedure 路径 | 归属 |
|----------|---------------------|------|
| QA_ENCODE | `/usr/data/ffmpeg/1920x1080_nv12.yuv` | tps-test → ffmpeg |
| QA_DECODE (pp) | `/usr/data/ffmpeg/1920x1080.mp4` | tps-test → ffmpeg |
| QA_DECODE (vdec) | `/usr/data/qa/1920x1080_h264_30fps.mp4` | tps-test → qa |
| QA_ENCODE/DECODE raw | `/usr/data/qa/*_nv12.yuv` 等 | OwnCloud → qa |

---

## 1. 两路同步方式

### OwnCloud → `/usr/data/qa/`

- Host: `http://owncloud.intchains.in:9000`
- 分享 token: `Z6zVDaVpjNrI80s` / 密码: `123456`
- 脚本：`taco-component-bitable-test/scripts/sync_qa_resources.sh`
- 板端：`cd /usr/data/qa && sh /root/sync_qa_resources.sh`
- 形态：**单个文件平铺**（无 zip/tar），主要为 `.yuv` / `.rgb` + `README.txt`

### tps-test → `/usr/data/ffmpeg/`（及部分 `/usr/data/qa/`）

- 安装：`apt-get install -y tps-test`
- `/usr/data/ffmpeg/`：编码输入 yuv/rgb、部分 jpg/mp4（约 55）
- `/usr/data/qa/`：解码用 `*_h264_*fps.mp4` / `*_h265_*fps.mp4` / 部分 jpg/ini（约 42）
- 形态：**单个文件**进 deb，不是资源压缩包

---

## 2. 本次新生成资源（Procedure 需要）

本机：`/tmp/qa_missing_assets/`  
板端（已临时放入）：`192.168.57.113:/usr/data/qa/`  
打包副本：`doc/qa-decode-bitable-20260716/new_assets/` + `new_qa_assets_20260717.tar.gz`

| 文件 | Procedure 路径 | 应入库 |
|------|----------------|--------|
| `48x48.jpg` 等 5 个 jpg | `/usr/data/qa/*.jpg` | **tps-test** 的 qa 段（与现有 `1920x1080.jpg` 同类） |
| `*_60fps.avi` 3 个 | `/usr/data/qa/*_60fps.avi` | **tps-test** 的 qa 段（与现有 `*_h264_*fps.mp4` 同类） |

**不是** OwnCloud 的 raw yuv/rgb；OwnCloud 公共分享亦只读（PUT 403）。  
入库形态：散文件进 deb，不要整包上传 OwnCloud。

本地散文件：`doc/qa-decode-bitable-20260716/new_assets/`
