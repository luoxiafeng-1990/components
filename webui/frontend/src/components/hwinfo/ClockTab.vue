<template>
  <div>
    <el-card shadow="hover" class="section">
      <template #header>
        <div class="card-header">
          <span>时钟信息</span>
          <el-button :icon="Refresh" circle size="small" @click="fetchData" :loading="loading" />
        </div>
      </template>

      <el-table
        v-if="data.clocks?.length"
        :data="data.clocks"
        size="small"
        stripe
        max-height="400"
      >
        <el-table-column prop="name" label="时钟名称" width="280" sortable />
        <el-table-column prop="rate" label="频率 (Hz)" width="150" sortable />
        <el-table-column prop="enable_count" label="启用计数" width="120" />
      </el-table>

      <div v-if="data.clk_summary" class="info-section">
        <div class="info-label">clk_summary</div>
        <pre class="raw-output">{{ data.clk_summary }}</pre>
      </div>

      <el-empty
        v-if="!data.clocks?.length && !data.clk_summary"
        description="无时钟信息 (可能需要 debugfs 挂载)"
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
    const res = await systemApi.clocks()
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
  max-height: 500px;
  overflow: auto;
  white-space: pre;
  margin: 0;
}
</style>
