<template>
  <el-container class="app-container">
    <el-aside width="200px" class="app-aside">
      <div class="logo">
        <el-icon :size="24"><Monitor /></el-icon>
        <span>Components</span>
      </div>
      <el-menu
        :default-active="currentRoute"
        router
        class="app-menu"
        background-color="#1d1e1f"
        text-color="#bbb"
        active-text-color="#409eff"
      >
        <el-menu-item index="/system">
          <el-icon><Platform /></el-icon>
          <span>硬件信息</span>
        </el-menu-item>
        <el-menu-item index="/datasources">
          <el-icon><FolderOpened /></el-icon>
          <span>数据源管理</span>
        </el-menu-item>
        <el-menu-item index="/workers">
          <el-icon><Setting /></el-icon>
          <span>Worker 管理</span>
        </el-menu-item>
        <el-menu-item index="/preview">
          <el-icon><VideoCamera /></el-icon>
          <span>实时预览</span>
        </el-menu-item>
      </el-menu>
    </el-aside>
    <el-main class="app-main">
      <router-view />
    </el-main>
  </el-container>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useRoute } from 'vue-router'
import { Monitor, Platform, FolderOpened, Setting, VideoCamera } from '@element-plus/icons-vue'

const route = useRoute()
const currentRoute = computed(() => route.path)
</script>

<style>
* { margin: 0; padding: 0; box-sizing: border-box; }

html, body, #app {
  height: 100%;
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
}

.app-container {
  height: 100vh;
}

.app-aside {
  background: #1d1e1f;
  border-right: 1px solid #333;
  overflow-y: auto;
}

.logo {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 20px 16px;
  color: #fff;
  font-size: 16px;
  font-weight: 600;
  border-bottom: 1px solid #333;
}

.app-menu {
  border-right: none !important;
}

.app-main {
  background: #f5f7fa;
  overflow-y: auto;
}
</style>
