<template>
  <!-- DISPLAY：全部参数 -->
  <template v-if="type === 'DISPLAY'">
    <el-form-item label="显示厂商">
      <el-select v-model="config.vendor" style="width:100%">
        <el-option value="tacopro" label="TacoPro (默认)" />
        <el-option value="taco" label="Taco" />
      </el-select>
    </el-form-item>
    <el-form-item label="刷新帧率">
      <el-input-number v-model="config.target_fps" :min="1" :max="120" />
    </el-form-item>
    <el-form-item label="OSD 叠加">
      <el-switch v-model="config.osd" />
    </el-form-item>
    <el-form-item label="OSD 刷新率">
      <el-input-number v-model="config.osd_fps" :min="1" :max="30" />
    </el-form-item>
    <el-form-item label="视图类型">
      <el-select v-model="config.view_type" style="width:100%" clearable placeholder="默认 grid">
        <el-option value="grid" label="网格 (grid)" />
        <el-option value="main_sidebar" label="主画面+侧栏 (main_sidebar)" />
      </el-select>
    </el-form-item>
    <el-form-item label="Slot 分配" v-if="config.view_type">
      <el-input v-model="config.slot_assignment" placeholder="如：0,1,2,3" />
    </el-form-item>
    <el-form-item label="主画面占比" v-if="config.view_type === 'main_sidebar'">
      <el-slider v-model="config.main_ratio" :min="0.3" :max="0.95" :step="0.05" show-input />
    </el-form-item>
    <el-form-item label="屏幕宽度">
      <el-input-number v-model="config.screen_width" :min="0" :max="7680" :step="1" />
    </el-form-item>
    <el-form-item label="屏幕高度">
      <el-input-number v-model="config.screen_height" :min="0" :max="4320" :step="1" />
    </el-form-item>
    <el-form-item label="BPP">
      <el-input-number v-model="config.bpp" :min="16" :max="64" />
    </el-form-item>
    <el-form-item label="帧宽度">
      <el-input-number v-model="config.frame_width" :min="0" :max="7680" />
    </el-form-item>
    <el-form-item label="帧高度">
      <el-input-number v-model="config.frame_height" :min="0" :max="4320" />
    </el-form-item>
  </template>

  <!-- NPU_INFERENCE：全部参数 -->
  <template v-else-if="type === 'NPU_INFERENCE'">
    <el-form-item label="模型路径" required>
      <el-input v-model="config.model_path" placeholder="/opt/models/yolov5.nb" />
    </el-form-item>
    <el-form-item label="置信度阈值">
      <el-slider v-model="config.conf_threshold" :min="0" :max="1" :step="0.05" show-input />
    </el-form-item>
    <el-form-item label="NMS 阈值">
      <el-slider v-model="config.nms_threshold" :min="0" :max="1" :step="0.05" show-input />
    </el-form-item>
    <el-form-item label="NPU 核心">
      <el-input-number v-model="config.npu_core" :min="0" :max="3" />
    </el-form-item>
    <el-form-item label="物理地址零拷贝">
      <el-switch v-model="config.physical_addr" />
    </el-form-item>
    <el-form-item label="绘制检测框">
      <el-switch v-model="config.draw" />
    </el-form-item>
    <el-form-item label="推理间隔(每N帧)">
      <el-input-number v-model="config.inference_interval" :min="1" :max="100" />
    </el-form-item>
  </template>

  <!-- JPEG_PREVIEW：全部参数 -->
  <template v-else-if="type === 'JPEG_PREVIEW'">
    <el-form-item label="编码器">
      <el-select v-model="config.encoder_name" style="width:100%">
        <el-option value="jpeg_taco" label="jpeg_taco (硬件)" />
        <el-option value="mjpeg" label="mjpeg (软件)" />
      </el-select>
    </el-form-item>
    <el-form-item label="JPEG 质量">
      <el-slider v-model="config.quality" :min="1" :max="100" show-input />
    </el-form-item>
    <el-form-item label="目标帧率">
      <el-input-number v-model="config.target_fps" :min="1" :max="60" />
    </el-form-item>
  </template>

  <!-- SAVE_RAW：全部参数 -->
  <template v-else-if="type === 'SAVE_RAW'">
    <el-form-item label="输出路径">
      <el-input v-model="config.output_path" placeholder="/data/output/frame" />
    </el-form-item>
    <el-form-item label="像素格式">
      <el-select v-model="config.format" style="width:100%" clearable>
        <el-option value="nv12" label="NV12" />
        <el-option value="nv21" label="NV21" />
        <el-option value="rgb888" label="RGB888" />
        <el-option value="bgr888" label="BGR888" />
        <el-option value="rgba8888" label="RGBA8888" />
        <el-option value="yuv420p" label="YUV420P" />
      </el-select>
    </el-form-item>
    <el-form-item label="保存帧数">
      <el-input-number v-model="config.frames" :min="1" :max="100000" />
    </el-form-item>
    <el-form-item label="解码方式">
      <el-select v-model="config.decoder" style="width:100%" clearable placeholder="默认 hw">
        <el-option value="hw" label="硬件 (hw)" />
        <el-option value="sw" label="软件 (sw)" />
      </el-select>
    </el-form-item>
  </template>

  <!-- SAVE_ENCODED：全部参数 -->
  <template v-else-if="type === 'SAVE_ENCODED'">
    <el-form-item label="输出路径">
      <el-input v-model="config.output_path" placeholder="/data/output/record.mp4" />
    </el-form-item>
    <el-form-item label="容器格式">
      <el-select v-model="config.format" style="width:100%" clearable>
        <el-option value="mp4" label="MP4" />
        <el-option value="mkv" label="MKV" />
        <el-option value="ts" label="TS" />
        <el-option value="flv" label="FLV" />
        <el-option value="avi" label="AVI" />
      </el-select>
    </el-form-item>
    <el-form-item label="录制时长(秒)">
      <el-input-number v-model="config.duration" :min="-1" />
    </el-form-item>
  </template>

  <!-- OPENCV：全部参数 -->
  <template v-else-if="type === 'OPENCV'">
    <el-form-item label="操作类型">
      <el-input v-model="config.case" placeholder="如 resize, crop, rotate..." />
    </el-form-item>
    <el-form-item label="操作参数">
      <el-input v-model="config.params" placeholder="操作相关参数" />
    </el-form-item>
    <el-form-item label="最大帧数">
      <el-input-number v-model="config.max_frames" :min="-1" />
    </el-form-item>
    <el-form-item label="PSNR">
      <el-switch v-model="config.psnr" />
    </el-form-item>
    <el-form-item label="SSIM">
      <el-switch v-model="config.ssim" />
    </el-form-item>
    <el-form-item label="详细日志">
      <el-switch v-model="config.verbose" />
    </el-form-item>
  </template>

  <!-- COUNT：无额外配置 -->
  <template v-else-if="type === 'COUNT'">
    <el-text type="info" size="small">帧计数消费者无需额外配置</el-text>
  </template>

  <!-- COMPARE：质量分析参数 -->
  <template v-else-if="type === 'COMPARE'">
    <el-form-item label="PSNR">
      <el-switch v-model="config.psnr" />
    </el-form-item>
    <el-form-item label="SSIM">
      <el-switch v-model="config.ssim" />
    </el-form-item>
    <el-form-item label="最小 PSNR" v-if="config.psnr">
      <el-input-number v-model="config.min_psnr" :min="0" :max="100" :step="1" />
    </el-form-item>
    <el-form-item label="最小 SSIM" v-if="config.ssim">
      <el-slider v-model="config.min_ssim" :min="0" :max="1" :step="0.01" show-input />
    </el-form-item>
  </template>

  <!-- 未知类型：通用 JSON -->
  <template v-else>
    <el-form-item label="配置 JSON">
      <el-input v-model="configJsonStr" type="textarea" :rows="4"
        placeholder='{"key": "value"}' @blur="parseConfigJson" />
    </el-form-item>
  </template>
</template>

<script setup lang="ts">
import { ref, watch } from 'vue'

const props = defineProps<{
  config: Record<string, any>
  type: string
}>()

const configJsonStr = ref(JSON.stringify(props.config || {}, null, 2))

watch(() => props.type, () => {
  configJsonStr.value = JSON.stringify(props.config || {}, null, 2)
})

function parseConfigJson() {
  try {
    const parsed = JSON.parse(configJsonStr.value)
    Object.assign(props.config, parsed)
  } catch { /* ignore parse errors */ }
}
</script>
