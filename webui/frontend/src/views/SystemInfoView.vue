<template>
  <div class="page-container">
    <div class="page-header">
      <h2>硬件信息</h2>
    </div>
    <el-tabs v-model="activeTab" type="border-card" class="hw-tabs">
      <el-tab-pane label="系统信息" name="system">
        <SystemTab />
      </el-tab-pane>
      <el-tab-pane label="板级配置" name="board" lazy>
        <BoardConfigTab />
      </el-tab-pane>
      <el-tab-pane label="设备树" name="dtmodules" lazy>
        <DeviceTreeModulesTab />
      </el-tab-pane>
      <el-tab-pane label="CPU" name="cpu" lazy>
        <CpuTab />
      </el-tab-pane>
      <el-tab-pane label="内存" name="memory" lazy>
        <MemoryTab />
      </el-tab-pane>
      <el-tab-pane label="NPU" name="npu" lazy>
        <NpuTab />
      </el-tab-pane>
      <el-tab-pane label="编码器" name="encoder" lazy>
        <HwModuleTab module="encoder" title="编码硬件信息" />
      </el-tab-pane>
      <el-tab-pane label="解码器" name="decoder" lazy>
        <HwModuleTab module="decoder" title="解码硬件信息" />
      </el-tab-pane>
      <el-tab-pane label="PP" name="pp" lazy>
        <HwModuleTab module="pp" title="PP 硬件信息" />
      </el-tab-pane>
      <el-tab-pane label="时钟" name="clocks" lazy>
        <ClockTab />
      </el-tab-pane>
      <el-tab-pane label="中断" name="interrupts" lazy>
        <InterruptTab />
      </el-tab-pane>
      <el-tab-pane label="GPIO" name="gpio" lazy>
        <GpioTab />
      </el-tab-pane>
    </el-tabs>
  </div>
</template>

<script setup lang="ts">
import { ref } from 'vue'
import SystemTab from '../components/hwinfo/SystemTab.vue'
import BoardConfigTab from '../components/hwinfo/BoardConfigTab.vue'
import DeviceTreeModulesTab from '../components/hwinfo/DeviceTreeModulesTab.vue'
import CpuTab from '../components/hwinfo/CpuTab.vue'
import MemoryTab from '../components/hwinfo/MemoryTab.vue'
import NpuTab from '../components/hwinfo/NpuTab.vue'
import HwModuleTab from '../components/hwinfo/HwModuleTab.vue'
import ClockTab from '../components/hwinfo/ClockTab.vue'
import InterruptTab from '../components/hwinfo/InterruptTab.vue'
import GpioTab from '../components/hwinfo/GpioTab.vue'

const activeTab = ref('system')
</script>

<style scoped>
.page-container { padding: 20px; }
.page-header {
  display: flex; justify-content: space-between; align-items: center;
  margin-bottom: 16px;
}
.page-header h2 { font-size: 20px; color: #303133; font-weight: 700; }
.hw-tabs { border-radius: 12px; overflow: hidden; }
.hw-tabs :deep(.el-tabs__content) { padding: 16px; min-height: 400px; background: #f8f9fb; }
.hw-tabs :deep(.el-tabs__header) { margin-bottom: 0; }
.hw-tabs :deep(.el-tabs__item) { font-weight: 500; }
.hw-tabs :deep(.el-tabs__item.is-active) { font-weight: 600; }
</style>
