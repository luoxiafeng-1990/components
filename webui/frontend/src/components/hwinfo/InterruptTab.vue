<template>
  <div>
    <el-card shadow="hover" class="section">
      <template #header>
        <div class="card-header">
          <span>中断信息 (/proc/interrupts)</span>
          <el-button :icon="Refresh" circle size="small" @click="fetchData" :loading="loading" />
        </div>
      </template>

      <el-table
        v-if="data.interrupts?.length"
        :data="data.interrupts"
        size="small"
        stripe
        max-height="600"
        :default-sort="{ prop: 'irq' }"
      >
        <el-table-column prop="irq" label="IRQ" width="80" fixed sortable />
        <el-table-column
          v-for="(h, idx) in (data.cpu_headers || [])"
          :key="h"
          :label="String(h)"
          width="100"
        >
          <template #default="{ row }">{{ row.counts?.[idx] ?? '' }}</template>
        </el-table-column>
        <el-table-column prop="description" label="描述" min-width="250" show-overflow-tooltip />
      </el-table>

      <el-empty v-else-if="!loading" description="无中断信息" />
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
    const res = await systemApi.interrupts()
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
</style>
