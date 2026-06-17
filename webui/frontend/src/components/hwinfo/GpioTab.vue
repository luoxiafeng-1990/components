<template>
  <div>
    <el-card shadow="hover" class="section">
      <template #header>
        <div class="card-header">
          <span>GPIO 信息</span>
          <el-button :icon="Refresh" circle size="small" @click="fetchData" :loading="loading" />
        </div>
      </template>

      <el-table
        v-if="data.chips?.length"
        :data="data.chips"
        size="small"
        stripe
      >
        <el-table-column prop="label" label="标签" width="200" />
        <el-table-column prop="base" label="Base" width="100" />
        <el-table-column prop="ngpio" label="GPIO 数量" width="120" />
        <el-table-column prop="path" label="路径" show-overflow-tooltip />
      </el-table>

      <div v-if="data.debug_output" class="info-section">
        <div class="info-label">GPIO 调试信息</div>
        <pre class="raw-output">{{ data.debug_output }}</pre>
      </div>

      <div v-if="data.gpioinfo" class="info-section">
        <div class="info-label">gpioinfo 输出</div>
        <pre class="raw-output">{{ data.gpioinfo }}</pre>
      </div>

      <div v-if="data.device_tree" class="info-section">
        <div class="info-label">设备树</div>
        <pre class="raw-output">{{ data.device_tree }}</pre>
      </div>

      <el-empty
        v-if="!loading && !data.chips?.length && !data.debug_output && !data.gpioinfo"
        description="无 GPIO 信息"
      />
    </el-card>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { Refresh } from '@element-plus/icons-vue'
import { systemApi } from '../../api'

const loading = ref(false)
const data = ref<Record<string, any>>({})

async function fetchData() {
  loading.value = true
  try {
    const res = await systemApi.gpio()
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
.info-section { margin-top: 16px; }
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
  max-height: 400px;
  overflow: auto;
  white-space: pre;
  margin: 0;
}
</style>
