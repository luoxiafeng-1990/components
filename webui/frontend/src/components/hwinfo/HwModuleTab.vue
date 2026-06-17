<template>
  <div>
    <el-card shadow="hover" class="section">
      <template #header>
        <div class="card-header">
          <span>{{ title }}</span>
          <el-button :icon="Refresh" circle size="small" @click="fetchData" :loading="loading" />
        </div>
      </template>

      <el-empty v-if="!loading && isEmpty" description="未检测到相关硬件信息" />

      <div v-else>
        <template v-for="(value, key) in data" :key="key">
          <div class="info-section" v-if="value && String(value).trim()">
            <div class="info-label">{{ labelMap[key as string] || key }}</div>
            <pre class="raw-output">{{ value }}</pre>
          </div>
        </template>
      </div>
    </el-card>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { Refresh } from '@element-plus/icons-vue'
import { systemApi } from '../../api'

const props = defineProps<{
  module: string
  title: string
}>()

const loading = ref(false)
const data = ref<Record<string, any>>({})

const labelMap: Record<string, string> = {
  tps_smi: 'TPS-SMI 输出',
  devices: '设备节点',
  driver: '驱动信息',
  device_tree: '设备树',
  device_tree_from_dtb: '设备树 (DTB 解析)',
  vpu_info: 'VPU 信息',
  v4l2: 'V4L2 设备',
  decode_fps: '解码 FPS',
  encode_fps: '编码 FPS',
}

const isEmpty = computed(() =>
  Object.values(data.value).every(v => !v || !String(v).trim())
)

async function fetchData() {
  loading.value = true
  try {
    const res = await systemApi.hwModule(props.module)
    data.value = res.data.data
  } catch { /* ignore */ }
  loading.value = false
}

onMounted(fetchData)
</script>

<style scoped>
.section { margin-bottom: 16px; }
.card-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  font-weight: 600;
}
.info-section { margin-bottom: 16px; }
.info-label {
  font-size: 13px;
  font-weight: 600;
  color: #606266;
  margin-bottom: 6px;
  padding-left: 8px;
  border-left: 3px solid #409eff;
}
.raw-output {
  background: #1e1e1e;
  color: #d4d4d4;
  padding: 12px;
  border-radius: 6px;
  font-family: 'Courier New', monospace;
  font-size: 12px;
  line-height: 1.5;
  max-height: 300px;
  overflow: auto;
  white-space: pre;
  margin: 0;
}
</style>
