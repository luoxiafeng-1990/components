# OSD 叠加层开发问题总结

本文档记录 OSD（On-Screen Display）叠加层从开发到稳定运行期间遇到的 5 个主要问题，包括现象、根因分析和修复方案。

OSD 基于 DSS overlay1（`/dev/fb2`），使用 FreeType 渲染文字，叠加在 overlay0 视频层之上，显示通道号、时间戳和帧率。

---

## 问题一：OSD 只显示在第一个通道，且有黑色背景

### 现象

- OSD 文字只出现在左上角一小块区域（第一个通道位置），其余 8 个通道没有 OSD
- OSD 区域背景是黑色的，遮挡了底层视频内容

### 根因

两个独立问题叠加：

1. **overlay1 分辨率被设备树限死为 480×270**（而不是 1920×1080）。DSS 只读取 OSD buffer 的前 480×270 像素，且步长（stride）是 480 而 OSD buffer 步长是 1920，导致文字显示乱码且仅覆盖左上角小区域。

2. **透明度方案不适用**。最初使用 DSS alpha blending 方案，但 taco DSS 硬件存在限制：
   - overlay0 的 `global_alpha` 写入返回 `EPERM`（不支持）
   - overlay1 的 `global_alpha` 期望 0-15 的值域而非 0-255
   - 导致两层都变成完全透明或完全不透明

### 修复

1. **修改设备树**：将 `tps-ea65xx-mes20-8a-v3.dts` 中 overlay-1 的分辨率从 480×270 改为 1920×1080，重编内核。

2. **透明度方案从 alpha blending 改为 colorkey（色键透明）**：
   - 关闭 `alpha_blending_enabled`
   - 设置 `trans_key_value = 0x00000000`（colorkey 值 = 黑色全透明）
   - 启用 `trans_key_enabled`
   - OSD buffer 清零后像素值 = colorkey → 该区域透明（显示底层视频）
   - 文字像素值 ≠ colorkey → 正常显示文字

### 涉及文件

| 文件 | 修改内容 |
|------|----------|
| `tps-ea65xx-mes20-8a-v3.dts` | overlay-1 分辨率改为 1920×1080 |
| `OsdOverlay.cpp` / `setupDssOverlay1()` | alpha blending → colorkey 方案 |

---

## 问题二：OSD 文字跳动/闪烁

### 现象

每次 OSD 刷新时，画面明显"跳"一下，视觉上不舒适。

### 根因

`renderOsd()` 的流程是：先 `clearBuffer()` 全 buffer 填 0 → 再逐个绘制文字。由于 OSD 直接写在**正在显示**的 DMA buffer 上：

```
时间线：
  t0: clearBuffer() → 整个 OSD 区域变透明（只看到视频）  ← 观众看到闪烁
  t1: drawText("CH1 ...") → 逐字渲染
  t2: drawText("CH2 ...") → 逐字渲染
  ...
  t9: 全部完成 → OSD 完整显示
```

`t0` 到 `t9` 之间有几毫秒的"空白期"，硬件每帧 VSYNC 都会读取 DMA buffer，此时读到的是清空或半成品的画面，表现为闪烁/跳动。

### 修复

引入 **BufferPool 4-buffer 翻页机制**（与视频层相同的管理方式）：

- OSD 的 4 个 DMA buffer 使用生产者-消费者队列管理
- 渲染线程从 free 队列取一个 buffer **在后台写入**（此 buffer 不在显示，观众看不到）
- 写完后提交到 filled 队列，通过 `FBIOPAN_DISPLAY` 翻页
- 翻页在 VSYNC 间隙完成（瞬间切换 DMA 地址），没有中间状态暴露给用户

```
时间线（修复后）：
  正在显示 buf[0]（完整 OSD 画面）
  后台: buf[1] clear → draw CH1 → draw CH2 → ... → draw CH9（观众看不到）
  翻页: FBIOPAN_DISPLAY 切到 buf[1]（瞬间完成，无闪烁）
```

### 涉及文件

| 文件 | 修改内容 |
|------|----------|
| `OsdOverlay.hpp` | 添加 BufferPool 成员，移除 shadow buffer |
| `OsdOverlay.cpp` | `init()` 中创建 BufferPool；`renderOsd()` 改为 acquireFree → 渲染 → releaseFilled → FBIOPAN |

---

## 问题三：启动花屏 + 不用 OSD 时也花屏

### 现象

- 启动时屏幕闪一下花屏（不管有没有 `--osd` 参数）
- 不加 `--osd` 参数运行时，视频内容也花屏/有随机噪点叠加

### 根因

设备树中 overlay-1 的 `status` 被设为 `"okay"`，导致 **overlay1 从开机就处于启用状态**。此时 overlay1 的 DMA 缓冲区内容是未初始化的随机数据，DSS 硬件每帧 VSYNC 都会将这些垃圾数据叠加到视频层上：

- **启动花屏**：开机后 overlay1 立即显示垃圾数据，直到 OSD 代码初始化并清空缓冲区
- **不用 OSD 时花屏**：overlay1 始终启用且没人清空/关闭它，垃圾数据一直叠加

### 修复

1. **修改设备树**：将 overlay-1 的 `status` 改回 `"disabled"`（保留 1920×1080 分辨率），重编内核。开机后 overlay1 默认关闭。

2. **代码保护**：在 `SharedDisplayContext::init()` 开头增加强制关闭 overlay1 的操作：

```cpp
// SharedDisplayContext::init() 开头
FILE* f = fopen("/sys/devices/platform/soc/soc:dss@c9200000/dss-overlay1/enabled", "w");
if (f) { fprintf(f, "0"); fclose(f); }
```

即使设备树配置有误，初始化时也会先强制关闭 overlay1，防止垃圾数据显示。

### 涉及文件

| 文件 | 修改内容 |
|------|----------|
| `tps-ea65xx-mes20-v2.dts` | overlay-1 `status` 改为 `"disabled"` |
| `SharedDisplayContext.cpp` | `init()` 开头强制写 `enabled=0` |

---

## 问题四：使用 --osd 启动时仍然短暂花屏

### 现象

问题三修复后，不用 OSD 不再花屏。但加了 `--osd` 启动时仍能看到一瞬间的花屏/闪烁。

### 根因

`OsdOverlay::setupDssOverlay1()` 中的执行顺序存在**竞态条件**：

```
原始顺序（有 bug）：
  1. write_sysfs("pixel_fmt", "argb8888")
  2. write_sysfs("enabled", "1")        ← overlay1 立刻显示！但此时 DMA 是垃圾数据
  3. ioctl(FB_IOCTL_SET_DMA_INFO)       ← 这时才绑定我们清零过的 DMA buffer
```

在第 2 步和第 3 步之间，overlay1 已经启用，但绑定的是内核默认的未初始化 DMA 内存，表现为短暂花屏。

### 修复

调整执行顺序 —— **先绑定已清零的 DMA buffer，最后才启用 overlay1**：

```
修复后顺序：
  1. write_sysfs("pixel_fmt", "argb8888")
  2. write_sysfs("trans_key_value", "0")
  3. write_sysfs("trans_key_enabled", "1")
  4. ioctl(FB_IOCTL_SET_DMA_INFO)       ← 先绑定 DMA（buffer 已清零）
  5. write_sysfs("enabled", "1")        ← 最后启用（此时显示的是清零后的空白层，colorkey 透明）
```

### 涉及文件

| 文件 | 修改内容 |
|------|----------|
| `OsdOverlay.cpp` / `setupDssOverlay1()` | 将 `ioctl(SET_DMA_INFO)` 移到 `enabled=1` 之前 |

---

## 问题五：OSD 运行中 SIGSEGV 崩溃

### 现象

运行一段时间后程序崩溃：

```
unhandled signal 11 code 0x1 at 0x0000000000000000
```

崩溃位置：`renderOsd()` 中的 `memset(pixels, 0, dma_mem_.frame_size)`，`pixels` 为 `NULL`。

### 根因

`BufferPool` 的 `releaseFilled()` 内部调用 `Buffer::freeBuffer()`，该函数的原始逻辑：

```cpp
void Buffer::freeBuffer() {
    if (avframe_) {
        av_frame_unref(avframe_);
    }
    virt_addr_ = nullptr;   // ← 无条件清空！
    // ...
}
```

OSD 的 Buffer 通过 `injectExternalBufferToPool()` 注入，`virt_addr_` 指向**固定的 DMA 物理映射地址**，没有 `avframe_`。但 `freeBuffer()` 无条件将 `virt_addr_` 置为 `nullptr`。下次 `acquireFree()` 取回这个 buffer 时，`getVirtualAddress()` 返回 NULL → `memset` 写入 NULL 地址 → 崩溃。

**本质问题**：`freeBuffer()` 的设计假设所有 buffer 的 `virt_addr_` 都来自 `AVFrame`（AVFrame 释放后地址失效），但 OSD 的 buffer 是外部注入的永久 DMA 映射，不会失效。

### 修复

将 `virt_addr_ = nullptr` 移入 `if (avframe_)` 块内，仅在有 AVFrame 时清空：

```cpp
void Buffer::freeBuffer() {
    if (avframe_) {
        av_frame_unref(avframe_);
        virt_addr_ = nullptr;   // AVFrame 数据清空后地址失效，置空
    }
    // 没有 avframe_ 的情况（如 OSD 的外部 DMA buffer），
    // virt_addr_ 是永久有效的映射地址，不能清空
    // ...
}
```

### 涉及文件

| 文件 | 修改内容 |
|------|----------|
| `Buffer.cpp` / `freeBuffer()` | `virt_addr_ = nullptr` 仅在 `avframe_` 存在时执行 |

---

## 总结

| 序号 | 问题 | 根因 | 修复方案 |
|:----:|------|------|----------|
| 1 | OSD 只在第一通道 + 黑背景 | 设备树限制 overlay1 为 480×270；alpha blending 不适用 | 设备树改 1920×1080；改用 colorkey 透明 |
| 2 | OSD 文字跳动闪烁 | 单 buffer 直写，clear 和 draw 之间有可见间隙 | BufferPool 4-buffer 翻页，后台渲染 + 瞬间切换 |
| 3 | 启动花屏 / 不用 OSD 也花屏 | 设备树 overlay1 `status="okay"`，开机即显示垃圾 | 设备树改 `disabled`；init 时强制关闭 overlay1 |
| 4 | 用 OSD 启动时短暂花屏 | `setupDssOverlay1` 先 enable 后绑 DMA，存在竞态 | 调整顺序：先绑 DMA → 最后 enable |
| 5 | OSD 运行中 SIGSEGV 崩溃 | `freeBuffer()` 无条件清空 `virt_addr_`，外部 DMA 地址丢失 | `virt_addr_=nullptr` 仅在有 `avframe_` 时执行 |
